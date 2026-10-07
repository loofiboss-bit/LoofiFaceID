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
#include <limits.h>
#include <poll.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_SOCKET_PATH "/run/kfaceauth/kfaceauthd.sock"
#define DAEMON_PROTOCOL_VERSION 2
#define OP_PAM_AUTH 0x10
#define STATUS_SUCCESS 0x00
#define STATUS_TIMEOUT 0x04
#define STATUS_DEVICE_BUSY 0x05
#define STATUS_INTERNAL_ERROR 0x06
#define STATUS_RATE_LIMITED 0x08
#define STATUS_PROGRESS_LOOKING_FOR_FACE 0x80
#define MAX_TIMEOUT_MS 2000

enum
{
    AUTH_TARGET_SDDM = 1,
    AUTH_TARGET_PLASMA_LOCK = 2,
};

static int auth_target_for_service(pam_handle_t *pamh, uint8_t *target)
{
    const void *service_item = NULL;
    if (pam_get_item(pamh, PAM_SERVICE, &service_item) != PAM_SUCCESS || service_item == NULL)
        return -1;

    const char *service = (const char *)service_item;
#ifdef KFACEAUTH_TEST_SOCKET_OVERRIDE
    const char *test_service = getenv("KFACEAUTH_PAM_SERVICE");
    if (test_service != NULL)
        service = test_service;
#endif

    if (strcmp(service, "sddm-kfaceauth") == 0)
    {
        *target = AUTH_TARGET_SDDM;
        return 0;
    }
    if (strcmp(service, "kde-kfaceauth") == 0)
    {
        *target = AUTH_TARGET_PLASMA_LOCK;
        return 0;
    }
    return -1;
}

static int fail_with_status(pam_handle_t *pamh, const char *status)
{
    (void)pam_info(pamh, "%s", status);
    closelog();
    return PAM_AUTH_ERR;
}

static int continue_with_password(pam_handle_t *pamh)
{
    return fail_with_status(pamh, "KFACEAUTH_STATUS=use-password");
}

static int daemon_unavailable(pam_handle_t *pamh, int socket_errno)
{
    return fail_with_status(pamh, socket_errno == ETIMEDOUT ? "KFACEAUTH_STATUS=timeout"
                                                            : "KFACEAUTH_STATUS=service-unavailable");
}

static int remaining_timeout_ms(const struct timespec *deadline, int *timeout_ms)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return -1;

    int64_t seconds = (int64_t)deadline->tv_sec - (int64_t)now.tv_sec;
    int64_t nanoseconds = (int64_t)deadline->tv_nsec - (int64_t)now.tv_nsec;
    if (nanoseconds < 0)
    {
        seconds -= 1;
        nanoseconds += INT64_C(1000000000);
    }
    if (seconds < 0 || (seconds == 0 && nanoseconds <= 0))
    {
        errno = ETIMEDOUT;
        return -1;
    }

    int64_t milliseconds = seconds * INT64_C(1000) + (nanoseconds + INT64_C(999999)) / INT64_C(1000000);
    if (milliseconds > INT_MAX)
        milliseconds = INT_MAX;
    if (milliseconds < 1)
        milliseconds = 1;
    *timeout_ms = (int)milliseconds;
    return 0;
}

static int wait_for_fd(int fd, short events, const struct timespec *deadline)
{
    for (;;)
    {
        int timeout_ms = 0;
        if (remaining_timeout_ms(deadline, &timeout_ms) != 0)
            return -1;

        struct pollfd descriptor;
        memset(&descriptor, 0, sizeof(descriptor));
        descriptor.fd = fd;
        descriptor.events = events;
        int result = poll(&descriptor, 1, timeout_ms);
        if (result < 0 && errno == EINTR)
            continue;
        if (result <= 0)
        {
            if (result == 0)
                errno = ETIMEDOUT;
            return -1;
        }
        if ((descriptor.revents & (POLLERR | POLLNVAL)) != 0)
        {
            errno = EIO;
            return -1;
        }
        if ((descriptor.revents & POLLHUP) != 0 && (events & POLLIN) == 0)
        {
            errno = ECONNRESET;
            return -1;
        }
        if ((descriptor.revents & events) != 0 || ((events & POLLIN) != 0 && (descriptor.revents & POLLHUP) != 0))
            return 0;
    }
}

static int connect_until(int fd, const struct sockaddr *address, socklen_t address_length,
                         const struct timespec *deadline)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
        return -1;
    if (connect(fd, address, address_length) == 0)
        return 0;
    if (errno != EINPROGRESS && errno != EAGAIN && errno != EWOULDBLOCK)
        return -1;
    if (wait_for_fd(fd, POLLOUT, deadline) != 0)
        return -1;

    int socket_error = 0;
    socklen_t error_length = (socklen_t)sizeof(socket_error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_length) != 0)
        return -1;
    if (socket_error != 0)
    {
        errno = socket_error;
        return -1;
    }
    return 0;
}

static int send_all_until(int fd, const uint8_t *buffer, size_t length, const struct timespec *deadline)
{
    size_t total = 0;
    while (total < length)
    {
        if (wait_for_fd(fd, POLLOUT, deadline) != 0)
            return -1;
        ssize_t n = send(fd, buffer + total, length - total, MSG_NOSIGNAL);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (n <= 0)
        {
            if (n == 0)
                errno = ECONNRESET;
            return -1;
        }
        total += (size_t)n;
    }
    return 0;
}

static int read_all_until(int fd, uint8_t *buffer, size_t length, const struct timespec *deadline)
{
    size_t total = 0;
    while (total < length)
    {
        if (wait_for_fd(fd, POLLIN, deadline) != 0)
            return -1;
        ssize_t n = recv(fd, buffer + total, length - total, 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
            continue;
        if (n <= 0)
        {
            if (n == 0)
                errno = ECONNRESET;
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

    uint8_t auth_target = 0;
    if (auth_target_for_service(pamh, &auth_target) != 0)
    {
        syslog(LOG_NOTICE, "unsupported PAM service; face authentication is disabled for this service");
        closelog();
        return PAM_AUTH_ERR;
    }

    syslog(LOG_DEBUG, "authentication request started");

    // Greeters can present a fixed, non-sensitive status through their PAM
    // conversation handler. The message contains no user-provided data.
    (void)pam_info(pamh, "KFACEAUTH_STATUS=starting-camera");

    const char *username = NULL;
    int pam_res = pam_get_user(pamh, &username, NULL);
    if (pam_res != PAM_SUCCESS || username == NULL || username[0] == '\0')
    {
        // Some Plasma lock-screen PAM callers omit PAM_USER; bind that request
        // to the account of the caller instead of accepting a caller-supplied UID.
        struct passwd *fallback_pw = getpwuid(getuid());
        if (fallback_pw != NULL && fallback_pw->pw_name != NULL)
        {
            username = fallback_pw->pw_name;
            syslog(LOG_DEBUG, "PAM user obtained from caller credentials");
        }
        else
        {
            syslog(LOG_NOTICE, "PAM user unavailable; continuing with password stack");
            return continue_with_password(pamh);
        }
    }
    else
    {
        syslog(LOG_DEBUG, "PAM user obtained from PAM stack");
    }

    struct passwd *pw = getpwnam(username);
    if (pw == NULL)
    {
        syslog(LOG_NOTICE, "PAM account lookup failed; continuing with password stack");
        return continue_with_password(pamh);
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
        const int socket_errno = errno;
        syslog(LOG_NOTICE, "daemon socket unavailable; continuing with password stack");
        return daemon_unavailable(pamh, socket_errno);
    }

    // Every socket operation shares one monotonic request deadline.
    struct timespec deadline;
    if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
    {
        const int socket_errno = errno;
        syslog(LOG_NOTICE, "monotonic clock unavailable; continuing with password stack");
        close(fd);
        return daemon_unavailable(pamh, socket_errno);
    }
    deadline.tv_sec += (time_t)(MAX_TIMEOUT_MS / 1000);
    deadline.tv_nsec += (long)((MAX_TIMEOUT_MS % 1000) * 1000000L);
    if (deadline.tv_nsec >= 1000000000L)
    {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t socket_path_len = strnlen(sock_path, sizeof(addr.sun_path));
    if (socket_path_len == 0 || socket_path_len >= sizeof(addr.sun_path))
    {
        syslog(LOG_NOTICE, "daemon socket path is invalid; continuing with password stack");
        close(fd);
        return fail_with_status(pamh, "KFACEAUTH_STATUS=service-unavailable");
    }
    memcpy(addr.sun_path, sock_path, socket_path_len + 1);

    if (connect_until(fd, (struct sockaddr *)&addr, (socklen_t)sizeof(addr), &deadline) != 0)
    {
        const int socket_errno = errno;
        syslog(LOG_NOTICE, "daemon unavailable; continuing with password stack");
        close(fd);
        // Fail closed silently for seamless password fallback
        return daemon_unavailable(pamh, socket_errno);
    }

    size_t user_len = strlen(username);
    if (user_len > 255)
    {
        syslog(LOG_NOTICE, "PAM account identifier exceeds protocol bound");
        close(fd);
        return continue_with_password(pamh);
    }

    uint32_t payload_len = (uint32_t)(14 + user_len);
    uint32_t frame_hdr = htobe32(payload_len);

    uint8_t req[4 + 14 + 256];
    memcpy(req, &frame_hdr, 4);

    uint16_t version_be = htobe16((uint16_t)DAEMON_PROTOCOL_VERSION);
    memcpy(req + 4, &version_be, 2);
    req[6] = OP_PAM_AUTH;
    req[7] = auth_target;

    uint32_t uid_be = htobe32(target_uid);
    memcpy(req + 8, &uid_be, 4);

    int remaining_ms = 0;
    if (remaining_timeout_ms(&deadline, &remaining_ms) != 0)
    {
        close(fd);
        return fail_with_status(pamh, "KFACEAUTH_STATUS=timeout");
    }
    uint32_t timeout_be = htobe32((uint32_t)remaining_ms);
    memcpy(req + 12, &timeout_be, 4);

    uint16_t ulen_be = htobe16((uint16_t)user_len);
    memcpy(req + 16, &ulen_be, 2);
    memcpy(req + 18, username, user_len);

    if (send_all_until(fd, req, 4 + payload_len, &deadline) != 0)
    {
        const int socket_errno = errno;
        syslog(LOG_NOTICE, "daemon request failed; continuing with password stack");
        close(fd);
        return daemon_unavailable(pamh, socket_errno);
    }

    uint8_t resp_body[4];
    for (;;)
    {
        uint8_t resp_hdr[4];
        if (read_all_until(fd, resp_hdr, sizeof(resp_hdr), &deadline) != 0)
        {
            const int socket_errno = errno;
            syslog(LOG_NOTICE, "daemon response unavailable; continuing with password stack");
            close(fd);
            return daemon_unavailable(pamh, socket_errno);
        }

        uint32_t resp_len_be = 0;
        memcpy(&resp_len_be, resp_hdr, sizeof(resp_len_be));
        uint32_t resp_len = be32toh(resp_len_be);
        if (resp_len != sizeof(resp_body))
        {
            syslog(LOG_NOTICE, "invalid daemon response; continuing with password stack");
            close(fd);
            return fail_with_status(pamh, "KFACEAUTH_STATUS=service-unavailable");
        }

        if (read_all_until(fd, resp_body, sizeof(resp_body), &deadline) != 0)
        {
            const int socket_errno = errno;
            syslog(LOG_NOTICE, "daemon response failed; continuing with password stack");
            close(fd);
            return daemon_unavailable(pamh, socket_errno);
        }

        uint16_t progress_version_be = 0;
        memcpy(&progress_version_be, resp_body, sizeof(progress_version_be));
        if (be16toh(progress_version_be) != DAEMON_PROTOCOL_VERSION || resp_body[3] != 0)
        {
            syslog(LOG_NOTICE, "invalid daemon response; continuing with password stack");
            close(fd);
            return fail_with_status(pamh, "KFACEAUTH_STATUS=service-unavailable");
        }
        if (resp_body[2] == STATUS_PROGRESS_LOOKING_FOR_FACE)
        {
            (void)pam_info(pamh, "KFACEAUTH_STATUS=looking-for-face");
            continue;
        }
        break;
    }

    close(fd);

    uint16_t resp_ver_be = 0;
    memcpy(&resp_ver_be, resp_body, sizeof(resp_ver_be));
    uint16_t resp_ver = be16toh(resp_ver_be);
    uint8_t resp_code = resp_body[2];

    if (resp_ver == DAEMON_PROTOCOL_VERSION && resp_code == STATUS_SUCCESS && resp_body[3] == 0)
    {
        syslog(LOG_NOTICE, "daemon returned positive authentication result");
        closelog();
        return PAM_SUCCESS;
    }

    syslog(LOG_NOTICE, "daemon did not return positive authentication; continuing with password stack");
    switch (resp_code)
    {
    case STATUS_TIMEOUT:
        return fail_with_status(pamh, "KFACEAUTH_STATUS=timeout");
    case STATUS_DEVICE_BUSY:
        return fail_with_status(pamh, "KFACEAUTH_STATUS=camera-busy");
    case STATUS_INTERNAL_ERROR:
        return fail_with_status(pamh, "KFACEAUTH_STATUS=service-unavailable");
    case STATUS_RATE_LIMITED:
        return fail_with_status(pamh, "KFACEAUTH_STATUS=retry-later");
    default:
        return continue_with_password(pamh);
    }
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
