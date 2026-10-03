// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define PAM_SM_AUTH
#define PAM_SM_ACCOUNT

#include <security/pam_ext.h>
#include <security/pam_modules.h>

#ifdef PAM_EXTERN
#undef PAM_EXTERN
#endif
#define PAM_EXTERN __attribute__((visibility("default"))) extern

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define DEFAULT_SOCKET_PATH "/run/kfaceauth/kfaceauthd.sock"
#define DAEMON_PROTOCOL_VERSION 1
#define OP_PAM_AUTH 0x10
#define STATUS_SUCCESS 0x00
#define MAX_TIMEOUT_MS 2000

static int send_all(int fd, const uint8_t *buffer, size_t length)
{
    size_t total = 0;
    while (total < length)
    {
        ssize_t n = write(fd, buffer + total, length - total);
        if (n <= 0)
        {
            if (n < 0 && errno == EINTR)
                continue;
            return -1;
        }
        total += (size_t)n;
    }
    return 0;
}

static int read_all(int fd, uint8_t *buffer, size_t length)
{
    size_t total = 0;
    while (total < length)
    {
        ssize_t n = read(fd, buffer + total, length - total);
        if (n <= 0)
        {
            if (n < 0 && errno == EINTR)
                continue;
            return -1;
        }
        total += (size_t)n;
    }
    return 0;
}

PAM_EXTERN int pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
    (void)flags;
    (void)argc;
    (void)argv;

    openlog("pam_kfaceauth", LOG_PID, LOG_AUTH);
    syslog(LOG_INFO, "pam_sm_authenticate called (pid=%d uid=%d euid=%d)",
           (int)getpid(), (int)getuid(), (int)geteuid());

    const char *username = NULL;
    int pam_res = pam_get_user(pamh, &username, NULL);
    if (pam_res != PAM_SUCCESS || username == NULL || username[0] == '\0')
    {
        // Fallback: try getpwuid(getuid()) for kscreenlocker_greet which
        // runs in the user's session
        struct passwd *fallback_pw = getpwuid(getuid());
        if (fallback_pw != NULL && fallback_pw->pw_name != NULL)
        {
            username = fallback_pw->pw_name;
            syslog(LOG_INFO, "pam_get_user failed (rc=%d), fallback to getpwuid: user=%s uid=%u",
                   pam_res, username, (unsigned)fallback_pw->pw_uid);
        }
        else
        {
            syslog(LOG_WARNING, "pam_get_user failed (rc=%d) and getpwuid fallback failed, returning PAM_USER_UNKNOWN",
                   pam_res);
            closelog();
            return PAM_USER_UNKNOWN;
        }
    }
    else
    {
        syslog(LOG_INFO, "pam_get_user succeeded: user=%s", username);
    }

    struct passwd *pw = getpwnam(username);
    if (pw == NULL)
    {
        syslog(LOG_WARNING, "getpwnam(%s) failed, returning PAM_USER_UNKNOWN", username);
        closelog();
        return PAM_USER_UNKNOWN;
    }
    uint32_t target_uid = (uint32_t)pw->pw_uid;
    syslog(LOG_INFO, "resolved user=%s to uid=%u", username, (unsigned)target_uid);

#ifdef KFACEAUTH_TEST_SOCKET_OVERRIDE
    const char *sock_path = getenv("KFACEAUTH_SOCKET_PATH");
    if (sock_path == NULL || sock_path[0] == '\0')
    {
        sock_path = DEFAULT_SOCKET_PATH;
    }
#else
    const char *sock_path = DEFAULT_SOCKET_PATH;
#endif

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
    {
        syslog(LOG_ERR, "socket() failed: %s", strerror(errno));
        closelog();
        return PAM_AUTH_ERR;
    }

    // Gate 4.2: Enforce strict 2.0-second timeout on all socket communication
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, (socklen_t)sizeof(tv)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, (socklen_t)sizeof(tv)) != 0)
    {
        syslog(LOG_ERR, "setsockopt timeout failed: %s", strerror(errno));
        close(fd);
        closelog();
        return PAM_AUTH_ERR;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    syslog(LOG_INFO, "connecting to daemon at %s", sock_path);
    if (connect(fd, (struct sockaddr *)&addr, (socklen_t)sizeof(addr)) != 0)
    {
        syslog(LOG_WARNING, "connect(%s) failed: %s — falling back to password", sock_path, strerror(errno));
        close(fd);
        closelog();
        // Fail closed silently for seamless password fallback
        return PAM_AUTH_ERR;
    }
    syslog(LOG_INFO, "connected to daemon socket");

    size_t user_len = strlen(username);
    if (user_len > 255)
    {
        syslog(LOG_ERR, "username too long (%zu), returning PAM_AUTH_ERR", user_len);
        close(fd);
        closelog();
        return PAM_AUTH_ERR;
    }

    uint32_t payload_len = (uint32_t)(14 + user_len);
    uint32_t frame_hdr = htobe32(payload_len);

    uint8_t req[4 + 14 + 256];
    memcpy(req, &frame_hdr, 4);

    uint16_t version_be = htobe16((uint16_t)DAEMON_PROTOCOL_VERSION);
    memcpy(req + 4, &version_be, 2);
    req[6] = OP_PAM_AUTH;
    req[7] = 0;

    uint32_t uid_be = htobe32(target_uid);
    memcpy(req + 8, &uid_be, 4);

    uint32_t timeout_be = htobe32(MAX_TIMEOUT_MS);
    memcpy(req + 12, &timeout_be, 4);

    uint16_t ulen_be = htobe16((uint16_t)user_len);
    memcpy(req + 16, &ulen_be, 2);
    memcpy(req + 18, username, user_len);

    syslog(LOG_INFO, "sending OP_PAM_AUTH: uid=%u user=%s timeout=%ums",
           (unsigned)target_uid, username, MAX_TIMEOUT_MS);

    if (send_all(fd, req, 4 + payload_len) != 0)
    {
        syslog(LOG_ERR, "send_all failed: %s", strerror(errno));
        close(fd);
        closelog();
        return PAM_AUTH_ERR;
    }

    uint8_t resp_hdr[4];
    if (read_all(fd, resp_hdr, 4) != 0)
    {
        syslog(LOG_ERR, "read response header failed: %s", strerror(errno));
        close(fd);
        closelog();
        return PAM_AUTH_ERR;
    }

    uint32_t resp_len = be32toh(*(uint32_t *)resp_hdr);
    if (resp_len < 4 || resp_len > 1024)
    {
        syslog(LOG_ERR, "invalid response length: %u", (unsigned)resp_len);
        close(fd);
        closelog();
        return PAM_AUTH_ERR;
    }

    uint8_t resp_body[1024];
    if (read_all(fd, resp_body, resp_len) != 0)
    {
        syslog(LOG_ERR, "read response body failed: %s", strerror(errno));
        close(fd);
        closelog();
        return PAM_AUTH_ERR;
    }

    close(fd);

    uint16_t resp_ver = be16toh(*(uint16_t *)resp_body);
    uint8_t resp_code = resp_body[2];

    syslog(LOG_INFO, "daemon response: version=%u status=%u (0=SUCCESS 1=AUTH_FAIL 2=ACCESS_DENIED 3=NO_PROFILE 4=TIMEOUT 5=DEVICE_BUSY)",
           (unsigned)resp_ver, (unsigned)resp_code);

    if (resp_ver == DAEMON_PROTOCOL_VERSION && resp_code == STATUS_SUCCESS)
    {
        syslog(LOG_INFO, "face authentication SUCCEEDED for user=%s uid=%u — returning PAM_SUCCESS",
               username, (unsigned)target_uid);
        closelog();
        return PAM_SUCCESS;
    }

    syslog(LOG_NOTICE, "face authentication FAILED for user=%s uid=%u (status=%u) — returning PAM_AUTH_ERR, password fallback",
           username, (unsigned)target_uid, (unsigned)resp_code);
    closelog();
    return PAM_AUTH_ERR;
}

PAM_EXTERN int pam_sm_setcred(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
    (void)pamh;
    (void)flags;
    (void)argc;
    (void)argv;
    return PAM_SUCCESS;
}

PAM_EXTERN int pam_sm_acct_mgmt(pam_handle_t *pamh, int flags, int argc, const char **argv)
{
    (void)pamh;
    (void)flags;
    (void)argc;
    (void)argv;
    return PAM_SUCCESS;
}
