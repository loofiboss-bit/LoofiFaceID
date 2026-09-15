// SPDX-License-Identifier: GPL-3.0-or-later

#include "camerapreviewsession.h"
#include "supportreport.h"
#include "systemstate.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

class SupportReportTest final : public QObject
{
    Q_OBJECT

  private Q_SLOTS:
    void mapsMilestoneIssuesToActions();
    void reportPreservesPreviewPrivacy();
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
