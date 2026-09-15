// SPDX-License-Identifier: GPL-3.0-or-later

#include <QElapsedTimer>
#include <QObject>
#include <QTest>

#include <security/pam_modules.h>

#include <endian.h>
#include <errno.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

extern "C"
{
    // Mock pam_get_user implementation to link with pam_kfaceauth
    int pam_get_user(pam_handle_t *pamh, const char **user, const char *prompt)
    {
        (void)prompt;
        *user = reinterpret_cast<const char *>(pamh);
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
    void testUnknownUserFailsClosed();
    void testMissingSocketFailsClosedImmediately();
    void testHungServerAbortsWithinTwoSeconds();
    void testMockServerSuccess();
    void testMockServerFailure();
    void testSetCredAndAcctMgmt();
};

void TestPam::testUnknownUserFailsClosed()
{
    const char *nonexistent = "kfaceauth_fake_user_404";
    int res = pam_sm_authenticate(reinterpret_cast<pam_handle_t *>(const_cast<char *>(nonexistent)), 0, 0, nullptr);
    QCOMPARE(res, PAM_USER_UNKNOWN);
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
                    // Send framed SUCCESS response: len = 4, ver = 1, status = 0, reserved = 0
                    uint32_t len_be = htobe32(4);
                    uint16_t ver_be = htobe16(1);
                    uint8_t resp[8];
                    memcpy(resp, &len_be, 4);
                    memcpy(resp + 4, &ver_be, 2);
                    resp[6] = 0; // STATUS_SUCCESS
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

    QCOMPARE(res, PAM_SUCCESS);
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
                    uint16_t ver_be = htobe16(1);
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
