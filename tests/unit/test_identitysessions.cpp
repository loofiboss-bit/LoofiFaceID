// SPDX-License-Identifier: BSD-2-Clause

#include "authcomponents.h"
#include "camerapreviewsession.h"
#include "enrollmentsession.h"
#include "identityworkerclient.h"
#include "kwalletkeyprovider.h"
#include "localverificationsession.h"
#include "pamconfiguration.h"

#include <QElapsedTimer>
#include <QProcessEnvironment>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <memory>

#ifndef KFACEAUTH_FAKE_IDENTITY_WORKER_PATH
#error "KFACEAUTH_FAKE_IDENTITY_WORKER_PATH must be defined"
#endif
#ifndef KFACEAUTH_FAKE_PREVIEW_WORKER_PATH
#error "KFACEAUTH_FAKE_PREVIEW_WORKER_PATH must be defined"
#endif

namespace
{
QProcessEnvironment environmentFor(const QString &mode)
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("KFACEAUTH_TEST_MODE"), mode);
    return environment;
}

void startPreview(CameraPreviewSession *preview)
{
    preview->refreshDevices();
    QTRY_COMPARE(preview->state(), CameraPreviewSession::State::Ready);
    preview->startPreview();
    QTRY_COMPARE(preview->state(), CameraPreviewSession::State::Streaming);
    QTRY_VERIFY(preview->frameAvailable());
}

class FakeKeyProvider final : public KWalletKeyProvider
{
  public:
    State state = State::Available;
    QByteArray key = QByteArray(32, char(0x41));
    bool deferRead = false;
    int storeCalls = 0;
    int deleteCalls = 0;
    int cancelCalls = 0;

    State boundedState() const override
    {
        return state;
    }

    void requestKey(Completion completion) override
    {
        if (deferRead)
        {
            pending = std::move(completion);
            return;
        }
        complete(std::move(completion), state);
    }

    Result generateTransientKey() const override
    {
        return Result{State::Available, QByteArray(32, char(0x52))};
    }

    void storeKey(QByteArray candidate, Completion completion) override
    {
        ++storeCalls;
        candidate.fill(0);
        complete(std::move(completion), state == State::Unavailable ? State::Unavailable : State::Available);
    }

    void deleteKey(Completion completion) override
    {
        ++deleteCalls;
        complete(std::move(completion), State::Absent);
    }

    void cancel() override
    {
        ++cancelCalls;
        if (pending)
        {
            const auto completion = std::move(pending);
            pending = {};
            complete(completion, State::Cancelled);
        }
    }

  private:
    void complete(Completion completion, State resultState) const
    {
        if (!completion)
            return;
        completion(Result{resultState, resultState == State::Available ? key : QByteArray()});
    }

    Completion pending;
};
struct AuthFixture
{
    using Session = EnrollmentSession;
    using Readiness = Session::Readiness;
    Session::AuthStatusSnapshot status;
    int launchCount = 0;
    bool deferReadback = false;
    QStringList arguments;
    QByteArray input;
    Session::AuthOperationCompletion finishOperation;
    Session::AuthStatusCompletion finishReadback;
    CameraPreviewSession preview{QStringLiteral("/nonexistent/preview-worker"), nullptr};
    FakeKeyProvider keys;
    IdentityWorkerClient worker{QStringLiteral(KFACEAUTH_FAKE_IDENTITY_WORKER_PATH),
                                environmentFor(QStringLiteral("session-lifecycle")), nullptr};
    std::unique_ptr<Session> session;

    AuthFixture()
    {
        status.components = status.sddmApi = status.sddmTheme = status.sddmPam = status.plasmaApi = status.plasmaPam =
            status.daemon = Readiness::Available;
        status.systemProfile = Readiness::Missing;
        status.sddmMode = QStringLiteral("manual");
        status.plasmaMode = QStringLiteral("on-activity");
        session = std::make_unique<Session>(
            &preview, &worker, &keys,
            [this](Session::AuthStatusCompletion completion)
            {
                if (deferReadback)
                    finishReadback = std::move(completion);
                else
                    completion(status);
            },
            [this](QStringList args, QByteArray bytes, Session::AuthOperationCompletion completion)
            {
                ++launchCount;
                arguments = std::move(args);
                input = std::move(bytes);
                finishOperation = std::move(completion);
            });
        session->refreshProfileStatus();
    }
    ~AuthFixture()
    {
        input.fill('\0');
    }
};

} // namespace

class IdentitySessionsTest final : public QObject
{
    Q_OBJECT

  private Q_SLOTS:
    void unavailableWalletStatesFailClosed_data();
    void unavailableWalletStatesFailClosed();
    void verificationRateLimitAndLifecycleClearMatch();
    void verificationRejectsStaleWorkerResponse();
    void enrollmentCancellationClearsTransientSamples();
    void pageHideCancelsActiveEnrollmentWorker();
    void rejectedSampleKeepsEnrollmentAvailable();
    void failedReplacementPreservesPreviousProfile();
    void guidePhaseAndCaptureSignal();
    void syntheticLifecycleRunsOneHundredCycles();
    void enrollmentWaitsForPendingPolicyRead_data();
    void enrollmentWaitsForPendingPolicyRead();
    void authComponentsAvailabilityTracksInstalledRuntimeFiles();
    void managedPamConfigurationRequiresPasswordFallback();
    void sameModeSystemProfileResyncPreservesIndependentChoices();
    void authChangeCancellationRemainsVisibleAfterRefresh();
    void authResyncWalletFailure_data();
    void authResyncWalletFailure();
    void authMutationRequiresSuccessfulReadback_data();
    void authMutationRequiresSuccessfulReadback();
    void authReadbackIsBoundedAndDiscardsLateResults();
    void authModeChangeMustReadBackBothChoices();
    void unknownPolicyCannotBecomeAnOffOrSuccessfulSetting();
};

void IdentitySessionsTest::sameModeSystemProfileResyncPreservesIndependentChoices()
{
    AuthFixture fixture;
    QTRY_VERIFY(fixture.session->profileReady());
    QVERIFY(fixture.session->systemProfileCanSync());
    fixture.session->syncSystemProfile();
    QCOMPARE(fixture.launchCount, 1);
    QCOMPARE(fixture.arguments,
             QStringList({QStringLiteral("/usr/libexec/kfaceauth-sync-vault"), QStringLiteral("--resync-profile")}));
    QVERIFY(fixture.input.startsWith("KFAUTH01"));
    QVERIFY(fixture.input.endsWith(QByteArray(32, char(0x41))));
    QVERIFY(fixture.session->systemAuthBusy());
    fixture.session->syncSystemProfile();
    fixture.session->setSddmAuthMode(QStringLiteral("off"));
    QCOMPARE(fixture.launchCount, 1);
    fixture.status.systemProfile = AuthFixture::Readiness::Available;
    fixture.deferReadback = true;
    fixture.finishOperation(0, true);
    QCOMPARE(fixture.session->authOperationState(), EnrollmentSession::AuthOperationState::Checking);
    QCOMPARE(fixture.session->authOperationResult(), EnrollmentSession::AuthOperationResult::None);
    QVERIFY(fixture.session->systemAuthBusy());
    fixture.finishReadback(fixture.status);
    QCOMPARE(fixture.session->authOperationResult(), EnrollmentSession::AuthOperationResult::Success);
    QVERIFY(!fixture.session->systemAuthBusy());
    QCOMPARE(fixture.session->sddmAuthMode(), QStringLiteral("manual"));
    QCOMPARE(fixture.session->plasmaLockAuthMode(), QStringLiteral("on-activity"));
    QVERIFY(fixture.session->systemProfileFreshnessText().startsWith(QStringLiteral("Unknown:")));
    QVERIFY(!fixture.preview.previewActive());
}

void IdentitySessionsTest::authChangeCancellationRemainsVisibleAfterRefresh()
{
    AuthFixture fixture;
    QTRY_VERIFY(fixture.session->profileReady());
    fixture.session->syncSystemProfile();
    fixture.finishOperation(126, true);
    QCOMPARE(fixture.session->authOperationResult(), EnrollmentSession::AuthOperationResult::Cancelled);
    QCOMPARE(fixture.session->sddmAuthMode(), QStringLiteral("manual"));
    fixture.session->checkSystemAuthStatus();
    QCOMPARE(fixture.session->authOperationErrorCode(), QStringLiteral("auth-change-cancelled"));
    QVERIFY(!fixture.session->systemAuthBusy());
}

void IdentitySessionsTest::authResyncWalletFailure_data()
{
    QTest::addColumn<int>("walletState");
    QTest::newRow("locked") << int(KWalletKeyProvider::State::Locked);
    QTest::newRow("cancelled") << int(KWalletKeyProvider::State::Cancelled);
    QTest::newRow("unavailable") << int(KWalletKeyProvider::State::Unavailable);
    QTest::newRow("missing") << int(KWalletKeyProvider::State::Absent);
}

void IdentitySessionsTest::authResyncWalletFailure()
{
    QFETCH(int, walletState);
    AuthFixture fixture;
    QTRY_VERIFY(fixture.session->profileReady());
    fixture.keys.state = static_cast<KWalletKeyProvider::State>(walletState);
    fixture.session->syncSystemProfile();
    QCOMPARE(fixture.launchCount, 0);
    QCOMPARE(fixture.session->authOperationResult(), EnrollmentSession::AuthOperationResult::WalletUnavailable);
    QVERIFY(!fixture.session->systemAuthBusy());
    QCOMPARE(fixture.session->sddmAuthMode(), QStringLiteral("manual"));
}

void IdentitySessionsTest::authMutationRequiresSuccessfulReadback_data()
{
    QTest::addColumn<int>("exitCode");
    QTest::addColumn<int>("profileReadiness");
    QTest::addColumn<int>("expectedResult");
    QTest::newRow("helper-failed") << 1 << int(AuthFixture::Readiness::Available)
                                   << int(EnrollmentSession::AuthOperationResult::Failed);
    QTest::newRow("profile-still-missing")
        << 0 << int(AuthFixture::Readiness::Missing) << int(EnrollmentSession::AuthOperationResult::ReadbackFailed);
    QTest::newRow("profile-unreadable") << 0 << int(AuthFixture::Readiness::Unknown)
                                        << int(EnrollmentSession::AuthOperationResult::ReadbackFailed);
}

void IdentitySessionsTest::authMutationRequiresSuccessfulReadback()
{
    QFETCH(int, exitCode);
    QFETCH(int, profileReadiness);
    QFETCH(int, expectedResult);
    AuthFixture fixture;
    QTRY_VERIFY(fixture.session->profileReady());
    fixture.session->syncSystemProfile();
    fixture.status.systemProfile = static_cast<AuthFixture::Readiness>(profileReadiness);
    fixture.finishOperation(exitCode, true);
    QCOMPARE(int(fixture.session->authOperationResult()), expectedResult);
    QVERIFY(!fixture.session->systemAuthBusy());
    const QString error = fixture.session->authOperationErrorCode();
    fixture.session->checkSystemAuthStatus();
    QCOMPARE(fixture.session->authOperationErrorCode(), error);
}

void IdentitySessionsTest::authReadbackIsBoundedAndDiscardsLateResults()
{
    AuthFixture fixture;
    QTRY_VERIFY(fixture.session->profileReady());
    fixture.session->syncSystemProfile();
    fixture.deferReadback = true;
    fixture.finishOperation(0, true);
    QTRY_COMPARE_WITH_TIMEOUT(fixture.session->authOperationResult(),
                              EnrollmentSession::AuthOperationResult::ReadbackFailed, 3500);
    QVERIFY(!fixture.session->systemAuthBusy());
    fixture.status.systemProfile = AuthFixture::Readiness::Available;
    QCOMPARE(fixture.session->sddmAuthMode(), QStringLiteral("unknown"));
    QVERIFY(!fixture.session->systemProfileCanSync());
    fixture.finishReadback(fixture.status);
    QCOMPARE(fixture.session->authOperationResult(), EnrollmentSession::AuthOperationResult::ReadbackFailed);
}

void IdentitySessionsTest::authModeChangeMustReadBackBothChoices()
{
    AuthFixture fixture;
    QTRY_VERIFY(fixture.session->profileReady());
    fixture.session->setSddmAuthMode(QStringLiteral("on-activity"));
    QCOMPARE(fixture.launchCount, 1);
    QVERIFY(fixture.arguments.contains(QStringLiteral("--enable-target")));
    fixture.status.sddmMode = QStringLiteral("on-activity");
    fixture.status.plasmaMode = QStringLiteral("off");
    fixture.status.systemProfile = AuthFixture::Readiness::Available;
    fixture.finishOperation(0, true);
    QCOMPARE(fixture.session->authOperationResult(), EnrollmentSession::AuthOperationResult::ReadbackFailed);
    QCOMPARE(fixture.session->plasmaLockAuthMode(), QStringLiteral("off"));
}

void IdentitySessionsTest::unknownPolicyCannotBecomeAnOffOrSuccessfulSetting()
{
    AuthFixture fixture;
    QTRY_VERIFY(fixture.session->profileReady());
    fixture.status.sddmMode = QStringLiteral("mode=off\nextra-private-output");
    fixture.session->checkSystemAuthStatus();
    QCOMPARE(fixture.session->sddmAuthMode(), QStringLiteral("unknown"));
    QVERIFY(!fixture.session->systemProfileCanSync());
    fixture.session->setSddmAuthMode(QStringLiteral("off"));
    QCOMPARE(fixture.launchCount, 0);
    QCOMPARE(fixture.session->authOperationResult(), EnrollmentSession::AuthOperationResult::NotReady);
}

void IdentitySessionsTest::authComponentsAvailabilityTracksInstalledRuntimeFiles()
{
    QVERIFY(!KFaceAuth::authComponentsAvailable(false, true, true));
    QVERIFY(!KFaceAuth::authComponentsAvailable(true, false, false));
    QVERIFY(KFaceAuth::authComponentsAvailable(true, true, false));
    QVERIFY(KFaceAuth::authComponentsAvailable(true, false, true));
}

void IdentitySessionsTest::managedPamConfigurationRequiresPasswordFallback()
{
    const QByteArray valid = "auth required pam_selinux_permit.so\n"
                             "# BEGIN kfaceauth experimental authentication\n"
                             "auth        sufficient    pam_kfaceauth.so\n"
                             "# END kfaceauth experimental authentication\n"
                             "auth substack password-auth\n";
    QVERIFY(KFaceAuth::hasManagedPamAuthBlock(valid));

    QVERIFY(!KFaceAuth::hasManagedPamAuthBlock("auth        sufficient    pam_kfaceauth.so\n"
                                               "auth substack password-auth\n"));
    QVERIFY(!KFaceAuth::hasManagedPamAuthBlock("# BEGIN kfaceauth experimental authentication\n"
                                               "auth        sufficient    pam_kfaceauth.so\n"
                                               "# END kfaceauth experimental authentication\n"));
    QVERIFY(!KFaceAuth::hasManagedPamAuthBlock("# BEGIN kfaceauth experimental authentication\n"
                                               "auth required pam_unix.so\n"
                                               "# END kfaceauth experimental authentication\n"
                                               "auth substack password-auth\n"));
}

void IdentitySessionsTest::unavailableWalletStatesFailClosed_data()
{
    QTest::addColumn<int>("keyState");
    QTest::addColumn<int>("expectedResult");
    QTest::newRow("locked") << int(KWalletKeyProvider::State::Locked)
                            << int(LocalVerificationSession::Result::VaultLocked);
    QTest::newRow("cancelled") << int(KWalletKeyProvider::State::Cancelled)
                               << int(LocalVerificationSession::Result::VaultLocked);
    QTest::newRow("unavailable") << int(KWalletKeyProvider::State::Unavailable)
                                 << int(LocalVerificationSession::Result::Unavailable);
    QTest::newRow("absent") << int(KWalletKeyProvider::State::Absent)
                            << int(LocalVerificationSession::Result::NoProfile);
}

void IdentitySessionsTest::unavailableWalletStatesFailClosed()
{
    QFETCH(int, keyState);
    QFETCH(int, expectedResult);
    CameraPreviewSession preview(QStringLiteral(KFACEAUTH_FAKE_PREVIEW_WORKER_PATH), nullptr);
    IdentityWorkerClient worker(QStringLiteral(KFACEAUTH_FAKE_IDENTITY_WORKER_PATH),
                                environmentFor(QStringLiteral("session")), this);
    FakeKeyProvider keys;
    keys.state = static_cast<KWalletKeyProvider::State>(keyState);
    LocalVerificationSession verification(&preview, &worker, &keys);
    startPreview(&preview);
    verification.setPageActive(true);

    verification.verifyCurrentFrame();

    QTRY_COMPARE(int(verification.result()), expectedResult);
    QVERIFY(!worker.busy());
    QCOMPARE(keys.storeCalls, 0);
    QCOMPARE(keys.deleteCalls, 0);
}

void IdentitySessionsTest::verificationRateLimitAndLifecycleClearMatch()
{
    CameraPreviewSession preview(QStringLiteral(KFACEAUTH_FAKE_PREVIEW_WORKER_PATH), nullptr);
    IdentityWorkerClient worker(QStringLiteral(KFACEAUTH_FAKE_IDENTITY_WORKER_PATH),
                                environmentFor(QStringLiteral("session")), this);
    FakeKeyProvider keys;
    LocalVerificationSession verification(&preview, &worker, &keys);
    startPreview(&preview);
    verification.setPageActive(true);

    verification.verifyCurrentFrame();
    QTRY_COMPARE(verification.result(), LocalVerificationSession::Result::Match);
    verification.verifyCurrentFrame();
    QCOMPARE(verification.state(), LocalVerificationSession::State::RateLimited);

    preview.stopPreview();
    QTRY_COMPARE(verification.result(), LocalVerificationSession::Result::None);
    QCOMPARE(verification.state(), LocalVerificationSession::State::Idle);
}

void IdentitySessionsTest::verificationRejectsStaleWorkerResponse()
{
    CameraPreviewSession preview(QStringLiteral(KFACEAUTH_FAKE_PREVIEW_WORKER_PATH), nullptr);
    IdentityWorkerClient worker(QStringLiteral(KFACEAUTH_FAKE_IDENTITY_WORKER_PATH),
                                environmentFor(QStringLiteral("stale")), this);
    FakeKeyProvider keys;
    LocalVerificationSession verification(&preview, &worker, &keys);
    startPreview(&preview);
    verification.setPageActive(true);

    verification.verifyCurrentFrame();

    QTRY_COMPARE(verification.result(), LocalVerificationSession::Result::InternalFailure);
    QCOMPARE(verification.errorCode(), QStringLiteral("identity-protocol-error"));
}

void IdentitySessionsTest::enrollmentCancellationClearsTransientSamples()
{
    CameraPreviewSession preview(QStringLiteral(KFACEAUTH_FAKE_PREVIEW_WORKER_PATH), nullptr);
    IdentityWorkerClient worker(QStringLiteral(KFACEAUTH_FAKE_IDENTITY_WORKER_PATH),
                                environmentFor(QStringLiteral("session")), this);
    FakeKeyProvider keys;
    keys.state = KWalletKeyProvider::State::Absent;
    EnrollmentSession enrollment(&preview, &worker, &keys);
    startPreview(&preview);
    enrollment.setPageActive(true);
    QTRY_COMPARE(enrollment.profileState(), EnrollmentSession::ProfileState::Absent);

    enrollment.startEnrollment();
    QTRY_COMPARE(enrollment.state(), EnrollmentSession::State::Enrolling);
    QCOMPARE(enrollment.remainingSeconds(), preview.remainingSeconds());
    QCOMPARE(preview.remainingSeconds(), PreviewProtocol::MaxEnrollmentSeconds);
    enrollment.captureSample();
    QTRY_COMPARE(enrollment.sampleCount(), 1);
    enrollment.cancel();

    QCOMPARE(enrollment.state(), EnrollmentSession::State::Cancelled);
    QCOMPARE(enrollment.sampleCount(), 0);
    QCOMPARE(keys.storeCalls, 0);
    QCOMPARE(keys.deleteCalls, 0);
}

void IdentitySessionsTest::pageHideCancelsActiveEnrollmentWorker()
{
    CameraPreviewSession preview(QStringLiteral(KFACEAUTH_FAKE_PREVIEW_WORKER_PATH), nullptr);
    IdentityWorkerClient worker(QStringLiteral(KFACEAUTH_FAKE_IDENTITY_WORKER_PATH),
                                environmentFor(QStringLiteral("session-hang-capture")), this);
    FakeKeyProvider keys;
    EnrollmentSession enrollment(&preview, &worker, &keys);
    startPreview(&preview);
    enrollment.setPageActive(true);
    QTRY_COMPARE(enrollment.profileState(), EnrollmentSession::ProfileState::Absent);
    enrollment.startEnrollment();
    QTRY_COMPARE(enrollment.state(), EnrollmentSession::State::Enrolling);
    QCOMPARE(enrollment.remainingSeconds(), preview.remainingSeconds());
    QCOMPARE(preview.remainingSeconds(), PreviewProtocol::MaxEnrollmentSeconds);
    enrollment.captureSample();
    QTRY_COMPARE(enrollment.state(), EnrollmentSession::State::Capturing);

    enrollment.setPageActive(false);

    QTRY_COMPARE(enrollment.state(), EnrollmentSession::State::Cancelled);
    QTRY_VERIFY(!worker.busy());
    QCOMPARE(enrollment.sampleCount(), 0);
    QCOMPARE(keys.storeCalls, 0);
}

void IdentitySessionsTest::rejectedSampleKeepsEnrollmentAvailable()
{
    QTemporaryDir markerDirectory;
    QVERIFY(markerDirectory.isValid());
    QProcessEnvironment environment = environmentFor(QStringLiteral("session-reject-sample-once"));
    environment.insert(QStringLiteral("KFACEAUTH_TEST_REJECT_MARKER"),
                       markerDirectory.filePath(QStringLiteral("sample-rejected")));
    CameraPreviewSession preview(QStringLiteral(KFACEAUTH_FAKE_PREVIEW_WORKER_PATH), nullptr);
    IdentityWorkerClient worker(QStringLiteral(KFACEAUTH_FAKE_IDENTITY_WORKER_PATH), environment, this);
    FakeKeyProvider keys;
    EnrollmentSession enrollment(&preview, &worker, &keys);
    startPreview(&preview);
    enrollment.setPageActive(true);
    QTRY_COMPARE(enrollment.profileState(), EnrollmentSession::ProfileState::Absent);
    enrollment.startEnrollment();
    QTRY_COMPARE(enrollment.state(), EnrollmentSession::State::Enrolling);

    enrollment.captureSample();
    QTRY_COMPARE(enrollment.errorCode(), QStringLiteral("identity-error-10"));
    QCOMPARE(enrollment.state(), EnrollmentSession::State::Enrolling);
    QCOMPARE(enrollment.sampleCount(), 0);
    QVERIFY(enrollment.canCapture());

    enrollment.captureSample();
    QTRY_COMPARE(enrollment.sampleCount(), 1);
    QCOMPARE(enrollment.state(), EnrollmentSession::State::Enrolling);
    QVERIFY(enrollment.errorCode().isEmpty());
    QCOMPARE(enrollment.guidePhase(), EnrollmentSession::GuidePhase::Left);
    enrollment.cancel();
}

void IdentitySessionsTest::failedReplacementPreservesPreviousProfile()
{
    CameraPreviewSession preview(QStringLiteral(KFACEAUTH_FAKE_PREVIEW_WORKER_PATH), nullptr);
    IdentityWorkerClient worker(QStringLiteral(KFACEAUTH_FAKE_IDENTITY_WORKER_PATH),
                                environmentFor(QStringLiteral("session-fail-commit")), this);
    FakeKeyProvider keys;
    EnrollmentSession enrollment(&preview, &worker, &keys);
    startPreview(&preview);
    enrollment.setPageActive(true);
    QTRY_COMPARE(enrollment.profileState(), EnrollmentSession::ProfileState::Ready);
    QCOMPARE(enrollment.storedSampleCount(), 5);

    enrollment.startEnrollment();
    QTRY_COMPARE(enrollment.state(), EnrollmentSession::State::Enrolling);
    for (int sample = 0; sample < enrollment.minimumSamples(); ++sample)
    {
        enrollment.captureSample();
        QTRY_COMPARE(enrollment.sampleCount(), sample + 1);
    }
    enrollment.finishAndSave();

    QTRY_COMPARE(enrollment.state(), EnrollmentSession::State::Failed);
    QVERIFY(enrollment.profileReady());
    QCOMPARE(enrollment.storedSampleCount(), 5);
    QVERIFY(!worker.busy());
    enrollment.setPageActive(false);
    preview.stopPreview();
    QTRY_COMPARE(preview.state(), CameraPreviewSession::State::Ready);
}

void IdentitySessionsTest::guidePhaseAndCaptureSignal()
{
    CameraPreviewSession preview(QStringLiteral(KFACEAUTH_FAKE_PREVIEW_WORKER_PATH), nullptr);
    IdentityWorkerClient worker(QStringLiteral(KFACEAUTH_FAKE_IDENTITY_WORKER_PATH),
                                environmentFor(QStringLiteral("session")), this);
    FakeKeyProvider keys;
    EnrollmentSession enrollment(&preview, &worker, &keys);
    startPreview(&preview);
    enrollment.setPageActive(true);
    QTRY_VERIFY(enrollment.profileState() != EnrollmentSession::ProfileState::Unknown &&
                enrollment.profileState() != EnrollmentSession::ProfileState::Checking);
    enrollment.startEnrollment();
    QTRY_COMPARE(enrollment.state(), EnrollmentSession::State::Enrolling);
    QCOMPARE(enrollment.guidePhase(), EnrollmentSession::GuidePhase::Frontal);

    QSignalSpy captured(&enrollment, &EnrollmentSession::sampleCaptured);
    enrollment.captureSample(true);
    QTRY_COMPARE(enrollment.sampleCount(), 1);
    QCOMPARE(enrollment.guidePhase(), EnrollmentSession::GuidePhase::Left);
    QCOMPARE(captured.count(), 1);
    QCOMPARE(captured.at(0).at(0).toInt(), 0);
    QCOMPARE(captured.at(0).at(1).toBool(), true);
    enrollment.cancel();
}

void IdentitySessionsTest::enrollmentWaitsForPendingPolicyRead_data()
{
    QTest::addColumn<QString>("outcome");
    QTest::newRow("complete") << QStringLiteral("complete");
    QTest::newRow("cancel") << QStringLiteral("cancel");
    QTest::newRow("timeout") << QStringLiteral("timeout");
    QTest::newRow("unknown") << QStringLiteral("unknown");
}

void IdentitySessionsTest::enrollmentWaitsForPendingPolicyRead()
{
    QFETCH(QString, outcome);
    CameraPreviewSession preview(QStringLiteral(KFACEAUTH_FAKE_PREVIEW_WORKER_PATH), nullptr);
    IdentityWorkerClient worker(QStringLiteral(KFACEAUTH_FAKE_IDENTITY_WORKER_PATH),
                                environmentFor(QStringLiteral("session-lifecycle")), this);
    FakeKeyProvider keys;
    EnrollmentSession::AuthStatusSnapshot status;
    status.components = EnrollmentSession::Readiness::Available;
    status.sddmMode = status.plasmaMode = QStringLiteral("off");
    status.systemProfile = EnrollmentSession::Readiness::Missing;
    bool deferred = false;
    EnrollmentSession::AuthStatusCompletion pending;
    EnrollmentSession enrollment(&preview, &worker, &keys,
                                 [&](EnrollmentSession::AuthStatusCompletion completion)
                                 {
                                     if (deferred)
                                         pending = std::move(completion);
                                     else
                                         completion(status);
                                 },
                                 {});
    startPreview(&preview);
    enrollment.setPageActive(true);
    QTRY_COMPARE(enrollment.profileState(), EnrollmentSession::ProfileState::Ready);
    enrollment.startEnrollment();
    QTRY_COMPARE(enrollment.state(), EnrollmentSession::State::Enrolling);
    for (int sample = 0; sample < enrollment.recommendedSamples(); ++sample)
    {
        enrollment.captureSample();
        QTRY_COMPARE(enrollment.sampleCount(), sample + 1);
    }
    deferred = true;
    enrollment.checkSystemAuthStatus();
    QVERIFY(pending);
    enrollment.finishAndSave();
    QCOMPARE(enrollment.state(), EnrollmentSession::State::Saving);
    if (outcome == QLatin1String("cancel"))
        enrollment.cancel();
    else if (outcome == QLatin1String("timeout"))
        QTRY_COMPARE_WITH_TIMEOUT(enrollment.state(), EnrollmentSession::State::Failed, 3000);
    else if (outcome == QLatin1String("unknown"))
        status.sddmMode = QStringLiteral("unknown");
    auto completion = std::move(pending);
    deferred = false;
    completion(status);
    const auto expected = outcome == QLatin1String("complete") ? EnrollmentSession::State::Complete
                          : outcome == QLatin1String("cancel") ? EnrollmentSession::State::Cancelled
                                                               : EnrollmentSession::State::Failed;
    QTRY_COMPARE(enrollment.state(), expected);
    QTest::qWait(30);
    QCOMPARE(enrollment.state(), expected);
}

void IdentitySessionsTest::syntheticLifecycleRunsOneHundredCycles()
{
    CameraPreviewSession preview(QStringLiteral(KFACEAUTH_FAKE_PREVIEW_WORKER_PATH), nullptr);
    IdentityWorkerClient worker(QStringLiteral(KFACEAUTH_FAKE_IDENTITY_WORKER_PATH),
                                environmentFor(QStringLiteral("session-lifecycle")), this);
    FakeKeyProvider keys;
    QElapsedTimer timer;
    timer.start();

    for (int cycle = 0; cycle < 100; ++cycle)
    {
        startPreview(&preview);
        EnrollmentSession enrollment(&preview, &worker, &keys,
                                     [](EnrollmentSession::AuthStatusCompletion completion)
                                     {
                                         EnrollmentSession::AuthStatusSnapshot status;
                                         status.sddmMode = status.plasmaMode = QStringLiteral("off");
                                         status.systemProfile = EnrollmentSession::Readiness::Missing;
                                         completion(status);
                                     },
                                     {});
        enrollment.setPageActive(true);
        QTRY_COMPARE(enrollment.profileState(), EnrollmentSession::ProfileState::Ready);
        enrollment.startEnrollment();
        QTRY_COMPARE(enrollment.state(), EnrollmentSession::State::Enrolling);

        for (int sample = 0; sample < enrollment.recommendedSamples(); ++sample)
        {
            QVERIFY(enrollment.canCapture());
            enrollment.captureSample();
            QTRY_COMPARE(enrollment.sampleCount(), sample + 1);
        }
        QVERIFY(enrollment.canFinish());
        enrollment.finishAndSave();
        QTRY_COMPARE(enrollment.state(), EnrollmentSession::State::Complete);
        QVERIFY(enrollment.profileReady());
        QCOMPARE(enrollment.storedSampleCount(), enrollment.recommendedSamples());

        enrollment.refreshProfileStatus();
        QTRY_COMPARE(enrollment.profileState(), EnrollmentSession::ProfileState::Ready);
        QCOMPARE(enrollment.storedSampleCount(), enrollment.recommendedSamples());

        LocalVerificationSession verification(&preview, &worker, &keys);
        verification.setPageActive(true);
        verification.verifyCurrentFrame();
        QTRY_COMPARE(verification.result(), LocalVerificationSession::Result::Match);
        QVERIFY(verification.isMatch());
        verification.clearResult();
        QVERIFY(!verification.hasResult());

        enrollment.deleteProfile();
        QTRY_COMPARE(enrollment.state(), EnrollmentSession::State::Complete);
        QTRY_COMPARE(enrollment.profileState(), EnrollmentSession::ProfileState::Absent);
        QCOMPARE(enrollment.storedSampleCount(), 0);
        QVERIFY(!worker.busy());

        verification.setPageActive(false);
        enrollment.setPageActive(false);
        preview.stopPreview();
        QTRY_COMPARE(preview.state(), CameraPreviewSession::State::Ready);
        QVERIFY(!preview.frameAvailable());
        QVERIFY(!worker.busy());

        QVERIFY2(timer.elapsed() < 120000,
                 qPrintable(
                     QStringLiteral("100-cycle synthetic lifecycle exceeded 120 seconds at cycle %1").arg(cycle + 1)));
    }
}

QTEST_MAIN(IdentitySessionsTest)

#include "test_identitysessions.moc"
