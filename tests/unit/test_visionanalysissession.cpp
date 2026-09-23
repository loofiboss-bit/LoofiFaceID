// SPDX-License-Identifier: GPL-3.0-or-later

#include "camerapreviewsession.h"
#include "visionanalysissession.h"

#include <QProcessEnvironment>
#include <QTest>

class VisionAnalysisSessionTest final : public QObject
{
    Q_OBJECT

  private Q_SLOTS:
    void successResponses_data();
    void successResponses();
    void startupFailure();
    void workerFailures_data();
    void workerFailures();
    void cancellationAndCleanup();
    void repeatedAnalyzeReplacesActiveRequest();
    void poseClassification_data();
    void poseClassification();
    void persistentGuidanceSession();
    void guidanceRecoversAfterRejectedEdgeResult();
    void guidanceStopsAfterRepeatedInvalidResults();

  private:
    static QProcessEnvironment environmentFor(const QString &mode);
    static void startPreview(CameraPreviewSession *preview);
};

QProcessEnvironment VisionAnalysisSessionTest::environmentFor(const QString &mode)
{
    QProcessEnvironment environment;
    environment.insert(QStringLiteral("KFACEAUTH_FAKE_VISION_MODE"), mode);
    return environment;
}

void VisionAnalysisSessionTest::startPreview(CameraPreviewSession *preview)
{
    preview->refreshDevices();
    QTRY_COMPARE(preview->state(), CameraPreviewSession::State::Ready);
    preview->startPreview();
    QTRY_COMPARE(preview->state(), CameraPreviewSession::State::Streaming);
    QTRY_VERIFY(preview->frameAvailable());
}

void VisionAnalysisSessionTest::successResponses_data()
{
    QTest::addColumn<QString>("mode");
    QTest::addColumn<VisionAnalysisSession::FaceFinding>("finding");
    QTest::newRow("zero") << QStringLiteral("zero") << VisionAnalysisSession::FaceFinding::NoFace;
    QTest::newRow("one") << QStringLiteral("one") << VisionAnalysisSession::FaceFinding::OneFace;
    QTest::newRow("multiple") << QStringLiteral("multiple") << VisionAnalysisSession::FaceFinding::MultipleFaces;
}

void VisionAnalysisSessionTest::successResponses()
{
    QFETCH(QString, mode);
    QFETCH(VisionAnalysisSession::FaceFinding, finding);
    CameraPreviewSession preview(QStringLiteral(KFACEAUTH_FAKE_PREVIEW_WORKER_PATH), nullptr);
    startPreview(&preview);
    VisionAnalysisSession analysis(&preview, QStringLiteral(KFACEAUTH_FAKE_VISION_WORKER_PATH), environmentFor(mode),
                                   nullptr);

    QVERIFY(analysis.canAnalyze());
    analysis.analyzeCurrentFrame();
    QTRY_COMPARE(analysis.state(), VisionAnalysisSession::State::Complete);
    QCOMPARE(analysis.faceFinding(), finding);
    QVERIFY(analysis.resultAvailable());
    QCOMPARE(analysis.brightness(), VisionAnalysisSession::Quality::Suitable);
    QCOMPARE(analysis.contrast(), VisionAnalysisSession::Quality::Suitable);
    QCOMPARE(analysis.sharpness(), VisionAnalysisSession::Quality::Suitable);
    if (finding == VisionAnalysisSession::FaceFinding::OneFace)
    {
        QCOMPARE(analysis.position(), VisionAnalysisSession::Position::Centered);
        QCOMPARE(analysis.distance(), VisionAnalysisSession::Distance::Suitable);
    }

    const quint64 completedGeneration = analysis.generation();
    analysis.cancelAnalysis();
    QCOMPARE(analysis.state(), VisionAnalysisSession::State::Idle);
    QVERIFY(!analysis.resultAvailable());
    QCOMPARE(analysis.faceFinding(), VisionAnalysisSession::FaceFinding::Unknown);
    QVERIFY(analysis.generation() > completedGeneration);
}

void VisionAnalysisSessionTest::startupFailure()
{
    CameraPreviewSession preview(QStringLiteral(KFACEAUTH_FAKE_PREVIEW_WORKER_PATH), nullptr);
    startPreview(&preview);
    VisionAnalysisSession analysis(&preview, QStringLiteral("/nonexistent/kfaceauth-vision-worker"), nullptr);
    analysis.analyzeCurrentFrame();
    QTRY_COMPARE(analysis.state(), VisionAnalysisSession::State::Failed);
    QCOMPARE(analysis.errorCode(), QStringLiteral("startup-failed"));
    QVERIFY(!analysis.resultAvailable());
}

void VisionAnalysisSessionTest::workerFailures_data()
{
    QTest::addColumn<QString>("mode");
    QTest::addColumn<QString>("errorCode");
    QTest::addColumn<int>("timeout");
    QTest::newRow("timeout") << QStringLiteral("timeout") << QStringLiteral("inference-timeout") << 6500;
    QTest::newRow("crash") << QStringLiteral("crash") << QStringLiteral("worker-crashed") << 2000;
    QTest::newRow("malformed") << QStringLiteral("malformed") << QStringLiteral("protocol-error") << 2000;
    QTest::newRow("unknown flags") << QStringLiteral("unknown-flags") << QStringLiteral("protocol-error") << 2000;
    QTest::newRow("contradictory flags") << QStringLiteral("contradictory-flags") << QStringLiteral("protocol-error")
                                         << 2000;
    QTest::newRow("landmark outside frame")
        << QStringLiteral("landmark-outside") << QStringLiteral("protocol-error") << 2000;
    QTest::newRow("landmark outside face rectangle")
        << QStringLiteral("landmark-outside-rect") << QStringLiteral("protocol-error") << 2000;
    QTest::newRow("oversized") << QStringLiteral("oversized") << QStringLiteral("protocol-error") << 3000;
    QTest::newRow("stale") << QStringLiteral("stale") << QStringLiteral("stale-response") << 2000;
    QTest::newRow("model unavailable") << QStringLiteral("model-unavailable") << QStringLiteral("analysis-error-11")
                                       << 2000;
    QTest::newRow("legacy internal error")
        << QStringLiteral("legacy-internal") << QStringLiteral("analysis-error-12") << 2000;
    QTest::newRow("invalid runtime output")
        << QStringLiteral("runtime-output") << QStringLiteral("analysis-error-14") << 2000;
    QTest::newRow("runtime failure") << QStringLiteral("runtime-failure") << QStringLiteral("analysis-error-15")
                                     << 2000;
    QTest::newRow("shutdown timeout") << QStringLiteral("shutdown-timeout") << QStringLiteral("shutdown-timeout")
                                      << 2500;
}

void VisionAnalysisSessionTest::workerFailures()
{
    QFETCH(QString, mode);
    QFETCH(QString, errorCode);
    QFETCH(int, timeout);
    CameraPreviewSession preview(QStringLiteral(KFACEAUTH_FAKE_PREVIEW_WORKER_PATH), nullptr);
    startPreview(&preview);
    VisionAnalysisSession analysis(&preview, QStringLiteral(KFACEAUTH_FAKE_VISION_WORKER_PATH), environmentFor(mode),
                                   nullptr);
    analysis.analyzeCurrentFrame();
    QTRY_COMPARE_WITH_TIMEOUT(analysis.state(), VisionAnalysisSession::State::Failed, timeout);
    QCOMPARE(analysis.errorCode(), errorCode);
    QVERIFY(!analysis.resultAvailable());
}

void VisionAnalysisSessionTest::cancellationAndCleanup()
{
    CameraPreviewSession preview(QStringLiteral(KFACEAUTH_FAKE_PREVIEW_WORKER_PATH), nullptr);
    startPreview(&preview);
    VisionAnalysisSession analysis(&preview, QStringLiteral(KFACEAUTH_FAKE_VISION_WORKER_PATH),
                                   environmentFor(QStringLiteral("timeout")), nullptr);
    analysis.analyzeCurrentFrame();
    QTRY_COMPARE(analysis.state(), VisionAnalysisSession::State::Analyzing);
    const quint64 activeGeneration = analysis.generation();
    analysis.cancelAnalysis();
    QCOMPARE(analysis.state(), VisionAnalysisSession::State::Idle);
    QVERIFY(analysis.generation() > activeGeneration);
    QVERIFY(!analysis.resultAvailable());
    QTRY_VERIFY(analysis.canAnalyze());

    analysis.analyzeCurrentFrame();
    QTRY_COMPARE(analysis.state(), VisionAnalysisSession::State::Analyzing);
    preview.stopPreview();
    QTRY_COMPARE(analysis.state(), VisionAnalysisSession::State::Idle);
    QVERIFY(!analysis.resultAvailable());
    QTRY_COMPARE(preview.state(), CameraPreviewSession::State::Ready);
}

void VisionAnalysisSessionTest::repeatedAnalyzeReplacesActiveRequest()
{
    CameraPreviewSession preview(QStringLiteral(KFACEAUTH_FAKE_PREVIEW_WORKER_PATH), nullptr);
    startPreview(&preview);
    VisionAnalysisSession analysis(&preview, QStringLiteral(KFACEAUTH_FAKE_VISION_WORKER_PATH),
                                   environmentFor(QStringLiteral("timeout")), nullptr);
    analysis.analyzeCurrentFrame();
    QTRY_COMPARE(analysis.state(), VisionAnalysisSession::State::Analyzing);
    const quint64 firstGeneration = analysis.generation();

    QVERIFY(analysis.canAnalyze());
    analysis.analyzeCurrentFrame();
    QVERIFY(analysis.generation() > firstGeneration);
    QTRY_COMPARE(analysis.state(), VisionAnalysisSession::State::Analyzing);
    QVERIFY(analysis.generation() > firstGeneration + 1);
    QVERIFY(!analysis.resultAvailable());

    analysis.cancelAnalysis();
    QCOMPARE(analysis.state(), VisionAnalysisSession::State::Idle);
    QVERIFY(!analysis.resultAvailable());
}

void VisionAnalysisSessionTest::poseClassification_data()
{
    QTest::addColumn<QString>("mode");
    QTest::addColumn<int>("sampleIndex");
    QTest::addColumn<VisionAnalysisSession::Pose>("pose");
    QTest::newRow("frontal") << QStringLiteral("one") << 0 << VisionAnalysisSession::Pose::Frontal;
    QTest::newRow("left") << QStringLiteral("left") << 1 << VisionAnalysisSession::Pose::Left;
    QTest::newRow("right") << QStringLiteral("right") << 2 << VisionAnalysisSession::Pose::Right;
    QTest::newRow("tilt") << QStringLiteral("tilt") << 3 << VisionAnalysisSession::Pose::Tilt;
    QTest::newRow("natural") << QStringLiteral("natural") << 4 << VisionAnalysisSession::Pose::Natural;
}

void VisionAnalysisSessionTest::poseClassification()
{
    QFETCH(QString, mode);
    QFETCH(int, sampleIndex);
    QFETCH(VisionAnalysisSession::Pose, pose);
    CameraPreviewSession preview(QStringLiteral(KFACEAUTH_FAKE_PREVIEW_WORKER_PATH), nullptr);
    startPreview(&preview);
    VisionAnalysisSession analysis(&preview, QStringLiteral(KFACEAUTH_FAKE_VISION_WORKER_PATH), environmentFor(mode),
                                   nullptr);
    analysis.analyzeCurrentFrame();
    QTRY_COMPARE(analysis.state(), VisionAnalysisSession::State::Complete);
    QCOMPARE(analysis.guidanceState(), VisionAnalysisSession::GuidanceState::Ready);
    QCOMPARE(analysis.detectedPose(), pose);
    QVERIFY(analysis.poseMatches(sampleIndex));
}

void VisionAnalysisSessionTest::persistentGuidanceSession()
{
    CameraPreviewSession preview(QStringLiteral(KFACEAUTH_FAKE_PREVIEW_WORKER_PATH), nullptr);
    startPreview(&preview);
    VisionAnalysisSession analysis(&preview, QStringLiteral(KFACEAUTH_FAKE_VISION_WORKER_PATH),
                                   environmentFor(QStringLiteral("one")), nullptr);

    analysis.startGuidance();
    QTRY_COMPARE(analysis.state(), VisionAnalysisSession::State::Complete);
    const quint64 firstGeneration = analysis.generation();
    QTRY_VERIFY_WITH_TIMEOUT(analysis.generation() > firstGeneration, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(
        analysis.state() == VisionAnalysisSession::State::Complete && analysis.generation() > firstGeneration, 3000);
    QCOMPARE(analysis.guidanceState(), VisionAnalysisSession::GuidanceState::Ready);
    QVERIFY(analysis.poseMatches(0));
    QVERIFY(!analysis.poseMatches(99));

    analysis.stopGuidance();
    QCOMPARE(analysis.state(), VisionAnalysisSession::State::Idle);
    QVERIFY(!analysis.continuousTracking());
}

void VisionAnalysisSessionTest::guidanceRecoversAfterRejectedEdgeResult()
{
    CameraPreviewSession preview(QStringLiteral(KFACEAUTH_FAKE_PREVIEW_WORKER_PATH), nullptr);
    startPreview(&preview);
    VisionAnalysisSession analysis(&preview, QStringLiteral(KFACEAUTH_FAKE_VISION_WORKER_PATH),
                                   environmentFor(QStringLiteral("edge-once")), nullptr);

    analysis.startGuidance();
    QTRY_COMPARE_WITH_TIMEOUT(analysis.errorCode(), QStringLiteral("analysis-error-13"), 2000);
    QVERIFY(analysis.continuousTracking());
    QVERIFY(!analysis.resultAvailable());
    const quint64 rejectedGeneration = analysis.generation();
    QTRY_VERIFY_WITH_TIMEOUT(analysis.generation() > rejectedGeneration, 2000);
    QTRY_VERIFY_WITH_TIMEOUT(analysis.state() == VisionAnalysisSession::State::Complete &&
                                 analysis.errorCode().isEmpty() &&
                                 analysis.guidanceState() == VisionAnalysisSession::GuidanceState::Ready,
                             2000);

    analysis.stopGuidance();
    QCOMPARE(analysis.state(), VisionAnalysisSession::State::Idle);
    QVERIFY(!analysis.continuousTracking());
}

void VisionAnalysisSessionTest::guidanceStopsAfterRepeatedInvalidResults()
{
    CameraPreviewSession preview(QStringLiteral(KFACEAUTH_FAKE_PREVIEW_WORKER_PATH), nullptr);
    startPreview(&preview);
    VisionAnalysisSession analysis(&preview, QStringLiteral(KFACEAUTH_FAKE_VISION_WORKER_PATH),
                                   environmentFor(QStringLiteral("edge-always")), nullptr);

    analysis.startGuidance();
    QTRY_COMPARE_WITH_TIMEOUT(analysis.state(), VisionAnalysisSession::State::Failed, 3000);
    QCOMPARE(analysis.errorCode(), QStringLiteral("analysis-error-13"));
    QVERIFY(!analysis.continuousTracking());
    QVERIFY(!analysis.resultAvailable());
}

QTEST_GUILESS_MAIN(VisionAnalysisSessionTest)

#include "test_visionanalysissession.moc"
