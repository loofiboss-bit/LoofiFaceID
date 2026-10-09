// SPDX-License-Identifier: GPL-3.0-or-later

#include "camerapreviewsession.h"
#include "enrollmentsession.h"
#include "identityworkerclient.h"
#include "kwalletkeyprovider.h"
#include "supportreport.h"
#include "systemstate.h"

#include <QDir>
#include <QFile>
#include <QMetaEnum>
#include <QTemporaryDir>
#include <QTest>

class SupportReportTest final : public QObject
{
    Q_OBJECT

  private Q_SLOTS:
    void mapsMilestoneIssuesToActions();
    void normalizesIdentityFailuresAndIgnoresWaiting();
    void reportPreservesPreviewPrivacy();
    void reportUsesTypedAuthenticationStatus();
    void authenticationReadinessDoesNotExportUntrustedModeOrIdentifiers();
    void exportsMarkdownAtomically();
};

void SupportReportTest::mapsMilestoneIssuesToActions()
{
    for (const QString &code : {
             QStringLiteral("camera-busy"),
             QStringLiteral("camera-unavailable"),
             QStringLiteral("native-engine-unavailable"),
             QStringLiteral("native-protocol-unavailable"),
             QStringLiteral("kwallet-locked"),
             QStringLiteral("vault-unavailable"),
             QStringLiteral("vault-unreadable"),
             QStringLiteral("vault-model-mismatch"),
             QStringLiteral("unsupported-platform"),
             QStringLiteral("model-unavailable"),
             QStringLiteral("worker-crashed"),
             QStringLiteral("worker-timeout"),
         })
    {
        QVERIFY2(!SupportReport::titleForCode(code).isEmpty(), qPrintable(code));
        QVERIFY2(!SupportReport::actionForCode(code).isEmpty(), qPrintable(code));
    }

    SystemState state;
    SupportReport report(&state);
    report.setTransientIssueCode(QStringLiteral("inference-timeout"));
    QCOMPARE(report.issueCode(), QStringLiteral("worker-timeout"));
    report.setTransientIssueCode(QStringLiteral("vault-key-unavailable"));
    QCOMPARE(report.issueCode(), QStringLiteral("kwallet-unavailable"));
}

void SupportReportTest::normalizesIdentityFailuresAndIgnoresWaiting()
{
    SystemState state;
    SupportReport report(&state);
    const QList<QPair<QString, QString>> cases = {
        {QStringLiteral("identity-error-13"), QStringLiteral("profile-unavailable")},
        {QStringLiteral("identity-error-14"), QStringLiteral("vault-locked")},
        {QStringLiteral("identity-error-16"), QStringLiteral("model-mismatch")},
        {QStringLiteral("identity-error-20"), QStringLiteral("model-unavailable")},
        {QStringLiteral("profile-unavailable"), QStringLiteral("profile-unavailable")},
        {QStringLiteral("frame-unavailable"), QStringLiteral("frame-unavailable")},
        {QStringLiteral("rate-limited"), QString()},
        {QStringLiteral("cancelled"), QString()},
        {QStringLiteral("identity-error-18"), QString()},
        {QStringLiteral("identity-error-999"), QString()},
    };
    for (const auto &entry : cases)
    {
        report.setTransientIssueCode(entry.first);
        QCOMPARE(report.issueCode(), entry.second);
        if (!entry.second.isEmpty())
        {
            QVERIFY(!report.issueTitle().isEmpty());
            QVERIFY(!report.recommendedAction().isEmpty());
        }
    }
}

void SupportReportTest::reportPreservesPreviewPrivacy()
{
    SystemStateSnapshot snapshot;
    snapshot.dataSource = QStringLiteral("/home/private-user/session");
    snapshot.fedoraVersion = QStringLiteral("password=hunter2");
    snapshot.plasmaVersion = QStringLiteral("owner@example.test");
    snapshot.engineVersion = QStringLiteral("embedding=biometric-payload");
    snapshot.issueCode = QStringLiteral("native-engine-unavailable");
    SystemState state;
    state.apply(snapshot);
    CameraPreviewSession previewSession(QStringLiteral(KFACEAUTH_FAKE_PREVIEW_WORKER_PATH), nullptr);
    SupportReport supportReport(&state, &previewSession);
    previewSession.refreshDevices();
    QTRY_COMPARE(previewSession.state(), CameraPreviewSession::State::Ready);

    const QString report = supportReport.report();
    QVERIFY(!report.contains(QStringLiteral("private-user")));
    QVERIFY(!report.contains(QStringLiteral("hunter2")));
    QVERIFY(!report.contains(QStringLiteral("owner@example.test")));
    QVERIFY(!report.contains(QStringLiteral("biometric-payload")));
    QVERIFY(!report.contains(QStringLiteral("/home/")));
    QVERIFY(report.contains(QStringLiteral("[redacted]")));
    QVERIFY(report.contains(QStringLiteral("- Distribution: unknown")));
    QVERIFY(report.contains(QStringLiteral("- Fedora version: [redacted]")));
    QVERIFY(report.contains(QStringLiteral("Native cameras: total=10 rgb=1 ir=1 unknown=8")));
    QVERIFY(!report.contains(QStringLiteral("RGB Test Camera")));
    QVERIFY(!report.contains(QStringLiteral("rgb-token")));
}

void SupportReportTest::reportUsesTypedAuthenticationStatus()
{
    SystemState state;
    CameraPreviewSession preview(QStringLiteral("/nonexistent/preview-worker"), nullptr);
    IdentityWorkerClient worker(QStringLiteral("/nonexistent/identity-worker"), {}, nullptr);
    KWalletKeyProvider keys;
    EnrollmentSession enrollment(&preview, &worker, &keys);
    SupportReport report(&state, &preview);
    report.setEnrollmentSession(&enrollment);
    const auto states = QMetaEnum::fromType<EnrollmentSession::AuthTargetStatus>();
    QVERIFY(report.report().contains(
        QStringLiteral("SDDM=%1; Plasma=%2")
            .arg(QString::fromLatin1(states.valueToKey(static_cast<int>(enrollment.sddmAuthStatus()))),
                 QString::fromLatin1(states.valueToKey(static_cast<int>(enrollment.plasmaLockAuthStatus()))))));
    QVERIFY(!report.report().contains(QStringLiteral("PAM configuration: Not implemented")));
    QVERIFY(!report.report().contains(QStringLiteral("Authentication decisions: Not implemented")));
    QVERIFY(report.report().contains(QStringLiteral("unqualified experiment")));
    report.setEnrollmentSession(nullptr);
    QVERIFY(report.report().contains(QStringLiteral("PAM configuration: Unavailable")));
}

void SupportReportTest::authenticationReadinessDoesNotExportUntrustedModeOrIdentifiers()
{
    EnrollmentSession::AuthStatusSnapshot snapshot;
    snapshot.components = EnrollmentSession::Readiness::Available;
    snapshot.sddmApi = EnrollmentSession::Readiness::Missing;
    snapshot.sddmMode = QStringLiteral("manual");
    snapshot.plasmaMode = QStringLiteral("manual\npassword=not-for-report");
    SystemState state;
    state.apply(SystemStateSnapshot{});
    CameraPreviewSession preview(QStringLiteral("/nonexistent/preview-worker"), nullptr);
    IdentityWorkerClient worker(QStringLiteral("/nonexistent/identity-worker"), {}, nullptr);
    KWalletKeyProvider keys;
    EnrollmentSession enrollment(&preview, &worker, &keys,
                                 [&snapshot](EnrollmentSession::AuthStatusCompletion completion)
                                 { completion(snapshot); }, {});
    SupportReport report(&state, &preview);
    report.setEnrollmentSession(&enrollment);
    QVERIFY(report.report().contains(QStringLiteral("sddm.policy: manual")));
    QVERIFY(report.report().contains(QStringLiteral("plasma-lock.policy: unknown")));
    QVERIFY(report.report().contains(QStringLiteral("sddm.integration: missing")));
    QVERIFY(report.report().contains(QStringLiteral("plasma-lock.theme-runtime: unknown")));
    QVERIFY(report.report().contains(QStringLiteral("profile-freshness: unknown")));
    QVERIFY(!report.report().contains(QStringLiteral("not-for-report")));
    QVERIFY(!report.report().contains(QStringLiteral("/home/")));
    for (const auto freshness :
         {EnrollmentSession::ProfileFreshness::Current, EnrollmentSession::ProfileFreshness::Stale,
          EnrollmentSession::ProfileFreshness::Unknown})
    {
        snapshot.freshness = freshness;
        enrollment.checkSystemAuthStatus();
        QVERIFY(
            report.report().contains(QStringLiteral("profile-freshness: ") + enrollment.systemProfileFreshnessCode()));
        QVERIFY(!report.report().contains(QStringLiteral("identity.freshness")));
        QVERIFY(!report.report().contains(QStringLiteral("sha256")));
    }
    QCOMPARE(report.issueCode(), QStringLiteral("sddm-api-missing"));
    QVERIFY(!report.recommendedAction().isEmpty());
    QVERIFY(!preview.previewActive());
}

void SupportReportTest::exportsMarkdownAtomically()
{
    SystemState state;
    state.apply(SystemStateSnapshot{});
    SupportReport supportReport(&state);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());

    QVERIFY(supportReport.exportToDirectory(directory.path()));
    const QStringList files = QDir(directory.path()).entryList({QStringLiteral("*.md")}, QDir::Files);
    QCOMPARE(files.size(), 1);
    QVERIFY(files.constFirst().startsWith(QStringLiteral("kfaceauth-support-")));
    QFile reportFile(QDir(directory.path()).filePath(files.constFirst()));
    QVERIFY(reportFile.open(QIODevice::ReadOnly | QIODevice::Text));
    const QByteArray contents = reportFile.readAll();
    QVERIFY(contents.startsWith("# KFaceAuth support report"));
    QVERIFY(!contents.contains("/home/"));
}

QTEST_MAIN(SupportReportTest)

#include "test_supportreport.moc"
