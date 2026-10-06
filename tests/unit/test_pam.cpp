// SPDX-License-Identifier: GPL-3.0-or-later

#include <QElapsedTimer>
#include <QObject>
#include <QTest>

#include <security/pam_modules.h>

#include <cstring>
#include <endian.h>
#include <errno.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

static int g_lookingForFaceStatusCount = 0;

extern "C"
{
    // Mock pam_get_user implementation to link with pam_kfaceauth
    int pam_get_user(pam_handle_t *pamh, const char **user, const char *prompt)
    {
        (void)prompt;
        *user = reinterpret_cast<const char *>(pamh);
        return PAM_SUCCESS;
    }

    int pam_get_item(const pam_handle_t *pamh, int item, const void **value)
    {
        (void)pamh;
        if (item != PAM_SERVICE || value == nullptr)
            return PAM_SYSTEM_ERR;
        const char *service = std::getenv("KFACEAUTH_PAM_SERVICE");
        *value = service == nullptr ? "sddm-kfaceauth" : service;
        return PAM_SUCCESS;
    }

    int pam_prompt(const pam_handle_t *pamh, int style, char **response, const char *fmt, ...)
    {
        (void)pamh;
        (void)response;
        if (style != PAM_TEXT_INFO)
            return PAM_SYSTEM_ERR;
        if (fmt != nullptr && std::strcmp(fmt, "KFACEAUTH_STATUS=looking-for-face") == 0)
            ++g_lookingForFaceStatusCount;
        return PAM_SUCCESS;
    }

    int pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc, const char **argv);
    int pam_sm_setcred(pam_handle_t *pamh, int flags, int argc, const char **argv);
    int pam_sm_acct_mgmt(pam_handle_t *pamh, int flags, int argc, const char **argv);
}

class TestPam : public QObject
{
    Q_OBJECT

  private Q_SLOTS:
    void testUnknownUserFallsBackToPassword();
    void testMissingSocketFailsClosedImmediately();
    void testPasswordServicesNeverStartFaceAuthentication();
    void testHungServerAbortsWithinTwoSeconds();
    void testSlowDripResponseUsesOneAbsoluteDeadline();
    void testMockServerSuccess();
    void testMockServerFailure();
    void testSetCredAndAcctMgmt();
};

void TestPam::testUnknownUserFallsBackToPassword()
{
    const char *nonexistent = "kfaceauth_fake_user_404";
    int res = pam_sm_authenticate(reinterpret_cast<pam_handle_t *>(const_cast<char *>(nonexistent)), 0, 0, nullptr);
    QCOMPARE(res, PAM_AUTH_ERR);
}

void TestPam::testMissingSocketFailsClosedImmediately()
{
    struct passwd *pw = getpwuid(getuid());
    QVERIFY(pw != nullptr);

    setenv("KFACEAUTH_SOCKET_PATH", "/tmp/kfaceauth_missing_test.sock", 1);

    QElapsedTimer timer;
    timer.start();

    int res = pam_sm_authenticate(reinterpret_cast<pam_handle_t *>(pw->pw_name), 0, 0, nullptr);
    QCOMPARE(res, PAM_AUTH_ERR);
    QVERIFY(timer.elapsed() < 500); // Must fail closed immediately
}

void TestPam::testPasswordServicesNeverStartFaceAuthentication()
{
    struct passwd *pw = getpwuid(getuid());
    QVERIFY(pw != nullptr);

    const char *sock_path = "/tmp/kfaceauth_unexpected_auth_test.sock";
    unlink(sock_path);
    setenv("KFACEAUTH_SOCKET_PATH", sock_path, 1);
    setenv("KFACEAUTH_PAM_SERVICE", "sddm", 1);

    QElapsedTimer timer;
    timer.start();
    const int result = pam_sm_authenticate(reinterpret_cast<pam_handle_t *>(pw->pw_name), 0, 0, nullptr);

    QCOMPARE(result, PAM_AUTH_ERR);
    QVERIFY(timer.elapsed() < 100);
    QVERIFY(access(sock_path, F_OK) != 0);
    unsetenv("KFACEAUTH_PAM_SERVICE");
}

void TestPam::testHungServerAbortsWithinTwoSeconds()
{
    // Gate 4.2 verification: PAM module aborts within <= 2.0s if camera is busy or server hangs
    struct passwd *pw = getpwuid(getuid());
    QVERIFY(pw != nullptr);

    const char *sock_path = "/tmp/kfaceauth_hung_test.sock";
    unlink(sock_path);

    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    QVERIFY(sfd >= 0);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    QCOMPARE(bind(sfd, (struct sockaddr *)&addr, sizeof(addr)), 0);
    QCOMPARE(listen(sfd, 1), 0);

    std::thread server_thread(
        [sfd]()
        {
            int cfd = accept(sfd, nullptr, nullptr);
            if (cfd >= 0)
            {
                // Sleep for 3 seconds without sending any response to trigger timeout
                usleep(3000000);
                close(cfd);
            }
            close(sfd);
        });

    setenv("KFACEAUTH_SOCKET_PATH", sock_path, 1);

    QElapsedTimer timer;
    timer.start();

    int res = pam_sm_authenticate(reinterpret_cast<pam_handle_t *>(pw->pw_name), 0, 0, nullptr);
    qint64 elapsed_ms = timer.elapsed();

    server_thread.join();
    unlink(sock_path);

    QCOMPARE(res, PAM_AUTH_ERR);
    // Strict Gate 4.2 deadline: should abort at ~2.0s (+/- 300ms tolerance for OS scheduler)
    QVERIFY2(elapsed_ms >= 1800 && elapsed_ms <= 2600,
             qPrintable(QStringLiteral("Elapsed time was %1 ms").arg(elapsed_ms)));
}

void TestPam::testSlowDripResponseUsesOneAbsoluteDeadline()
{
    struct passwd *pw = getpwuid(getuid());
    QVERIFY(pw != nullptr);

    const char *sock_path = "/tmp/kfaceauth_slow_drip_test.sock";
    unlink(sock_path);

    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    QVERIFY(sfd >= 0);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    QCOMPARE(bind(sfd, (struct sockaddr *)&addr, sizeof(addr)), 0);
    QCOMPARE(listen(sfd, 1), 0);

    std::thread server_thread(
        [sfd]()
        {
            int cfd = accept(sfd, nullptr, nullptr);
            if (cfd >= 0)
            {
                uint8_t request[512];
                (void)read(cfd, request, sizeof(request));
                const uint8_t response[] = {0, 0, 0, 4, 0, 1, 0, 0};
                for (uint8_t byte : response)
                {
                    usleep(300000);
                    if (send(cfd, &byte, 1, MSG_NOSIGNAL) != 1)
                        break;
                }
                close(cfd);
            }
            close(sfd);
        });

    setenv("KFACEAUTH_SOCKET_PATH", sock_path, 1);
    QElapsedTimer timer;
    timer.start();
    int res = pam_sm_authenticate(reinterpret_cast<pam_handle_t *>(pw->pw_name), 0, 0, nullptr);
    const qint64 elapsed_ms = timer.elapsed();

    server_thread.join();
    unlink(sock_path);

    QCOMPARE(res, PAM_AUTH_ERR);
    QVERIFY2(elapsed_ms >= 1800 && elapsed_ms <= 2600,
             qPrintable(QStringLiteral("Elapsed time was %1 ms").arg(elapsed_ms)));
}

void TestPam::testMockServerSuccess()
{
    struct passwd *pw = getpwuid(getuid());
    QVERIFY(pw != nullptr);

    const char *sock_path = "/tmp/kfaceauth_success_test.sock";
    unlink(sock_path);

    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    QVERIFY(sfd >= 0);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    QCOMPARE(bind(sfd, (struct sockaddr *)&addr, sizeof(addr)), 0);
    QCOMPARE(listen(sfd, 1), 0);

    std::thread server_thread(
        [sfd]()
        {
            int cfd = accept(sfd, nullptr, nullptr);
            if (cfd >= 0)
            {
                uint8_t req[512];
                ssize_t n = read(cfd, req, sizeof(req));
                if (n > 4)
                {
                    // Send one fixed progress event followed by the only success-bearing final response.
                    uint32_t len_be = htobe32(4);
                    uint16_t ver_be = htobe16(2);
                    uint8_t resp[16];
                    for (int frame = 0; frame < 2; ++frame)
                    {
                        memcpy(resp + frame * 8, &len_be, 4);
                        memcpy(resp + frame * 8 + 4, &ver_be, 2);
                        resp[frame * 8 + 6] = frame == 0 ? 0x80 : 0;
                        resp[frame * 8 + 7] = 0;
                    }
                    write(cfd, resp, sizeof(resp));
                }
                close(cfd);
            }
            close(sfd);
        });

    setenv("KFACEAUTH_SOCKET_PATH", sock_path, 1);

    int res = pam_sm_authenticate(reinterpret_cast<pam_handle_t *>(pw->pw_name), 0, 0, nullptr);

    server_thread.join();
    unlink(sock_path);

    QCOMPARE(res, PAM_SUCCESS);
    QCOMPARE(g_lookingForFaceStatusCount, 1);
}

void TestPam::testMockServerFailure()
{
    struct passwd *pw = getpwuid(getuid());
    QVERIFY(pw != nullptr);

    const char *sock_path = "/tmp/kfaceauth_fail_test.sock";
    unlink(sock_path);

    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    QVERIFY(sfd >= 0);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
    QCOMPARE(bind(sfd, (struct sockaddr *)&addr, sizeof(addr)), 0);
    QCOMPARE(listen(sfd, 1), 0);

    std::thread server_thread(
        [sfd]()
        {
            int cfd = accept(sfd, nullptr, nullptr);
            if (cfd >= 0)
            {
                uint8_t req[512];
                ssize_t n = read(cfd, req, sizeof(req));
                if (n > 4)
                {
                    // Send framed DEVICE_BUSY response (status = 5)
                    uint32_t len_be = htobe32(4);
                    uint16_t ver_be = htobe16(2);
                    uint8_t resp[8];
                    memcpy(resp, &len_be, 4);
                    memcpy(resp + 4, &ver_be, 2);
                    resp[6] = 5; // STATUS_DEVICE_BUSY
                    resp[7] = 0;
                    write(cfd, resp, sizeof(resp));
                }
                close(cfd);
            }
            close(sfd);
        });

    setenv("KFACEAUTH_SOCKET_PATH", sock_path, 1);

    int res = pam_sm_authenticate(reinterpret_cast<pam_handle_t *>(pw->pw_name), 0, 0, nullptr);

    server_thread.join();
    unlink(sock_path);

    QCOMPARE(res, PAM_AUTH_ERR);
}

void TestPam::testSetCredAndAcctMgmt()
{
    QCOMPARE(pam_sm_setcred(nullptr, 0, 0, nullptr), PAM_SUCCESS);
    QCOMPARE(pam_sm_acct_mgmt(nullptr, 0, 0, nullptr), PAM_SUCCESS);
}

QTEST_GUILESS_MAIN(TestPam)
#include "test_pam.moc"
