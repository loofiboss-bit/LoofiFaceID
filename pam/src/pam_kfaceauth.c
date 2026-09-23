// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#define PAM_SM_AUTH
#define PAM_SM_ACCOUNT

#include <security/pam_ext.h>
#include <security/pam_modules.h>

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

    const char *username = NULL;
    int pam_res = pam_get_user(pamh, &username, NULL);
    if (pam_res != PAM_SUCCESS || username == NULL || username[0] == '\0')
    {
        return PAM_USER_UNKNOWN;
    }

    struct passwd *pw = getpwnam(username);
    if (pw == NULL)
    {
        return PAM_USER_UNKNOWN;
    }
    uint32_t target_uid = (uint32_t)pw->pw_uid;

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
        return PAM_AUTH_ERR;
    }

    // Gate 4.2: Enforce strict 2.0-second timeout on all socket communication
    struct timeval tv;
    tv.tv_sec = 2;
    tv.tv_usec = 0;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, (socklen_t)sizeof(tv)) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, (socklen_t)sizeof(tv)) != 0)
    {
        close(fd);
        return PAM_AUTH_ERR;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, (socklen_t)sizeof(addr)) != 0)
    {
        close(fd);
        // Fail closed silently for seamless password fallback
        return PAM_AUTH_ERR;
    }

    size_t user_len = strlen(username);
    if (user_len > 255)
    {
        close(fd);
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

    if (send_all(fd, req, 4 + payload_len) != 0)
    {
        close(fd);
        return PAM_AUTH_ERR;
    }

    uint8_t resp_hdr[4];
    if (read_all(fd, resp_hdr, 4) != 0)
    {
        close(fd);
        return PAM_AUTH_ERR;
    }

    uint32_t resp_len = be32toh(*(uint32_t *)resp_hdr);
    if (resp_len < 4 || resp_len > 1024)
    {
        close(fd);
        return PAM_AUTH_ERR;
    }

    uint8_t resp_body[1024];
    if (read_all(fd, resp_body, resp_len) != 0)
    {
        close(fd);
        return PAM_AUTH_ERR;
    }

    close(fd);

    uint16_t resp_ver = be16toh(*(uint16_t *)resp_body);
    uint8_t resp_code = resp_body[2];

    if (resp_ver == DAEMON_PROTOCOL_VERSION && resp_code == STATUS_SUCCESS)
    {
        return PAM_SUCCESS;
    }

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
