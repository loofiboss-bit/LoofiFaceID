// SPDX-License-Identifier: GPL-3.0-or-later

#include "camerapreviewsession.h"

#include <QCborArray>
#include <QSignalSpy>
#include <QTest>

class CameraPreviewSessionTest final : public QObject
{
    Q_OBJECT

  private Q_SLOTS:
    void discoveryPreviewAndStop();
    void hotplugSnapshotsWaitForRelease();
    void hotplugPreservesTokensAcrossReorderingAndRequiresSelectionAfterLoss();
    void initialUniqueCameraCanBeSelectedAutomatically();
    void refreshPreservesExplicitSelection();
    void workerRestartRequiresExplicitSelection();
    void enrollmentBudgetIsGrantedOncePerPreview();
    void invalidSelectionIsIgnored();
    void stableFailures();
    void lifecycleFailures();
    void timeLimitHotUnplugAndRepeatedStart();
};

namespace
{
QCborMap cameraSnapshot(std::initializer_list<const char *> tokens)
{
    QCborArray devices;
    for (const char *token : tokens)
        devices.append(QCborMap{{QStringLiteral("token"), QString::fromLatin1(token)},
                                {QStringLiteral("label"), QStringLiteral("Camera")},
                                {QStringLiteral("spectrum"), QStringLiteral("rgb")}});
    return {{QStringLiteral("protocol"), PreviewProtocol::Version},
            {QStringLiteral("session"), QStringLiteral("test-session")},
            {QStringLiteral("sequence"), 1},
            {QStringLiteral("type"), QStringLiteral("devices")},
            {QStringLiteral("devices"), devices}};
}
} // namespace

void CameraPreviewSessionTest::hotplugSnapshotsWaitForRelease()
{
    for (const auto state : {CameraPreviewSession::State::Starting, CameraPreviewSession::State::Streaming,
                             CameraPreviewSession::State::Stopping})
    {
        CameraPreviewSession session(QStringLiteral("/unused"), nullptr);
        QVERIFY(session.handleDevices(cameraSnapshot({"first", "second"})));
        session.setSelectedDeviceIndex(1);
        session.m_state = state;
        QVERIFY(session.handleDevices(cameraSnapshot({"second", "first"})));
        QCOMPARE(session.state(), state);
        QCOMPARE(session.selectedDeviceIndex(), 1);
        QCOMPARE(session.m_devices.at(1).token, QStringLiteral("second"));
        session.m_sessionId = QStringLiteral("test-session");
        session.m_state = CameraPreviewSession::State::Stopping;
        QVERIFY(session.handleRecord({{QStringLiteral("protocol"), PreviewProtocol::Version},
                                      {QStringLiteral("session"), QStringLiteral("test-session")},
                                      {QStringLiteral("sequence"), 2},
                                      {QStringLiteral("type"), QStringLiteral("stopped")}}));
        QCOMPARE(session.state(), CameraPreviewSession::State::Ready);
        QCOMPARE(session.selectedDeviceIndex(), 0);
        QVERIFY(session.m_pendingDevices.isEmpty());
        QVERIFY(!session.frameAvailable());
    }
}

void CameraPreviewSessionTest::hotplugPreservesTokensAcrossReorderingAndRequiresSelectionAfterLoss()
{
    CameraPreviewSession session(QStringLiteral("/unused"), nullptr);
    QVERIFY(session.handleDevices(cameraSnapshot({"first", "second"})));
    session.setSelectedDeviceIndex(1);
    QVERIFY(session.handleDevices(cameraSnapshot({"second", "first"})));
    QCOMPARE(session.selectedDeviceIndex(), 0);
    QVERIFY(session.handleDevices(cameraSnapshot({"first"})));
    QCOMPARE(session.selectedDeviceIndex(), -1);
    QVERIFY(session.handleDevices(cameraSnapshot({"second", "first"})));
    QCOMPARE(session.selectedDeviceIndex(), -1);
    QVERIFY(session.handleDevices(cameraSnapshot({})));
    QCOMPARE(session.state(), CameraPreviewSession::State::Failed);
    QVERIFY(session.handleDevices(cameraSnapshot({"first"})));
    QCOMPARE(session.selectedDeviceIndex(), -1);
}

void CameraPreviewSessionTest::initialUniqueCameraCanBeSelectedAutomatically()
{
    CameraPreviewSession session(QStringLiteral("/unused"), nullptr);
    QVERIFY(session.handleDevices(cameraSnapshot({"first"})));
    QCOMPARE(session.selectedDeviceIndex(), 0);
    QVERIFY(session.canStartPreview());
    QVERIFY(!session.frameAvailable());
}

void CameraPreviewSessionTest::discoveryPreviewAndStop()
{
    CameraPreviewSession session(QStringLiteral(KFACEAUTH_FAKE_PREVIEW_WORKER_PATH), nullptr);
    session.refreshDevices();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Ready);
    QCOMPARE(session.deviceCount(), 10);
    QCOMPARE(session.data(session.index(0), CameraPreviewSession::LabelRole).toString(),
             QStringLiteral("RGB Test Camera"));

    session.setSelectedDeviceIndex(1);
    session.startPreview();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Streaming);
    QTRY_VERIFY(session.frameAvailable());
    QCOMPARE(session.spectrum(), QStringLiteral("ir"));
    QVERIFY(session.frame().width() <= PreviewProtocol::MaxWidth);
    QVERIFY(session.frame().height() <= PreviewProtocol::MaxHeight);

    session.stopPreview();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Ready);
    QVERIFY(!session.frameAvailable());
}

void CameraPreviewSessionTest::refreshPreservesExplicitSelection()
{
    CameraPreviewSession session(QStringLiteral(KFACEAUTH_FAKE_PREVIEW_WORKER_PATH), nullptr);
    session.refreshDevices();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Ready);
    QCOMPARE(session.selectedDeviceIndex(), -1);
    QVERIFY(!session.canStartPreview());
    session.setSelectedDeviceIndex(1);
    session.refreshDevices();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Ready);
    QCOMPARE(session.selectedDeviceIndex(), 1);
    QVERIFY(!session.frameAvailable());
}

void CameraPreviewSessionTest::workerRestartRequiresExplicitSelection()
{
    CameraPreviewSession session(QStringLiteral(KFACEAUTH_FAKE_PREVIEW_WORKER_PATH), nullptr);
    session.refreshDevices();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Ready);
    session.setSelectedDeviceIndex(3);
    session.startPreview();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Failed);
    session.refreshDevices();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Ready);
    QCOMPARE(session.selectedDeviceIndex(), -1);
    QVERIFY(!session.canStartPreview());
}

void CameraPreviewSessionTest::enrollmentBudgetIsGrantedOncePerPreview()
{
    CameraPreviewSession session(QStringLiteral(KFACEAUTH_FAKE_PREVIEW_WORKER_PATH), nullptr);
    QVERIFY(!session.beginEnrollmentBudget());
    session.refreshDevices();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Ready);
    session.setSelectedDeviceIndex(0);
    session.startPreview();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Streaming);
    QCOMPARE(session.remainingSeconds(), PreviewProtocol::MaxPreviewSeconds);
    QVERIFY(session.beginEnrollmentBudget());
    QCOMPARE(session.remainingSeconds(), PreviewProtocol::MaxEnrollmentSeconds);
    QVERIFY(!session.beginEnrollmentBudget());
    QTest::qWait(1100);
    QVERIFY(session.remainingSeconds() < PreviewProtocol::MaxEnrollmentSeconds);
    const int remaining = session.remainingSeconds();
    QVERIFY(!session.beginEnrollmentBudget());
    QCOMPARE(session.remainingSeconds(), remaining);
    session.stopPreview();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Ready);
    session.startPreview();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Streaming);
    QCOMPARE(session.remainingSeconds(), PreviewProtocol::MaxPreviewSeconds);
    QVERIFY(session.beginEnrollmentBudget());
    session.stopPreview();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Ready);
}

void CameraPreviewSessionTest::invalidSelectionIsIgnored()
{
    CameraPreviewSession session(QStringLiteral(KFACEAUTH_FAKE_PREVIEW_WORKER_PATH), nullptr);
    session.refreshDevices();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Ready);
    session.setSelectedDeviceIndex(99);
    QCOMPARE(session.selectedDeviceIndex(), -1);
    session.startPreview();
    QCOMPARE(session.state(), CameraPreviewSession::State::Ready);
}

namespace
{
void selectByTokenLabel(CameraPreviewSession &session, const QString &label)
{
    for (int index = 0; index < session.deviceCount(); ++index)
    {
        if (session.data(session.index(index), CameraPreviewSession::LabelRole).toString() == label)
        {
            session.setSelectedDeviceIndex(index);
            return;
        }
    }
    QFAIL(qPrintable(QStringLiteral("Missing fake device: %1").arg(label)));
}
} // namespace

void CameraPreviewSessionTest::stableFailures()
{
    CameraPreviewSession session(QStringLiteral(KFACEAUTH_FAKE_PREVIEW_WORKER_PATH), nullptr);
    session.refreshDevices();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Ready);

    selectByTokenLabel(session, QStringLiteral("Busy Test Camera"));
    session.startPreview();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Failed);
    QCOMPARE(session.errorCode(), QStringLiteral("camera-busy"));

    session.refreshDevices();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Ready);
    selectByTokenLabel(session, QStringLiteral("Protocol Test Camera"));
    session.startPreview();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Failed);
    QCOMPARE(session.errorCode(), QStringLiteral("protocol-error"));
    QVERIFY(!session.frameAvailable());
}

void CameraPreviewSessionTest::lifecycleFailures()
{
    CameraPreviewSession session(QStringLiteral(KFACEAUTH_FAKE_PREVIEW_WORKER_PATH), nullptr);
    session.refreshDevices();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Ready);
    selectByTokenLabel(session, QStringLiteral("Crash Test Camera"));
    session.startPreview();
    QTRY_COMPARE_WITH_TIMEOUT(session.state(), CameraPreviewSession::State::Failed, 3000);
    QCOMPARE(session.errorCode(), QStringLiteral("worker-crashed"));

    session.refreshDevices();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Ready);
    selectByTokenLabel(session, QStringLiteral("Startup Timeout Camera"));
    session.startPreview();
    QTRY_COMPARE_WITH_TIMEOUT(session.state(), CameraPreviewSession::State::Failed, 6000);
    QCOMPARE(session.errorCode(), QStringLiteral("startup-timeout"));

    session.refreshDevices();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Ready);
    selectByTokenLabel(session, QStringLiteral("Stalled Test Camera"));
    session.startPreview();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Streaming);
    QTRY_COMPARE_WITH_TIMEOUT(session.state(), CameraPreviewSession::State::Failed, 4000);
    QCOMPARE(session.errorCode(), QStringLiteral("stream-stalled"));

    session.refreshDevices();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Ready);
    selectByTokenLabel(session, QStringLiteral("Stop Timeout Camera"));
    session.startPreview();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Streaming);
    session.stopPreview();
    QTRY_COMPARE_WITH_TIMEOUT(session.state(), CameraPreviewSession::State::Failed, 2000);
    QCOMPARE(session.errorCode(), QStringLiteral("worker-crashed"));
}

void CameraPreviewSessionTest::timeLimitHotUnplugAndRepeatedStart()
{
    CameraPreviewSession session(QStringLiteral(KFACEAUTH_FAKE_PREVIEW_WORKER_PATH), nullptr);
    session.refreshDevices();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Ready);

    selectByTokenLabel(session, QStringLiteral("Time Limit Camera"));
    session.startPreview();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Streaming);
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Ready);
    QVERIFY(!session.frameAvailable());

    selectByTokenLabel(session, QStringLiteral("Hot Unplug Camera"));
    session.startPreview();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Streaming);
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Failed);
    QCOMPARE(session.errorCode(), QStringLiteral("no-camera"));
    QVERIFY(!session.frameAvailable());

    session.refreshDevices();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Ready);
    selectByTokenLabel(session, QStringLiteral("RGB Test Camera"));
    session.startPreview();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Streaming);
    session.startPreview();
    QCOMPARE(session.state(), CameraPreviewSession::State::Streaming);
    session.stopPreview();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Ready);
    session.startPreview();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Streaming);
    session.stopPreview();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Ready);
}

QTEST_GUILESS_MAIN(CameraPreviewSessionTest)

#include "test_camerapreviewsession.moc"
