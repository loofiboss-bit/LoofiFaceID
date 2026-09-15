// SPDX-License-Identifier: GPL-3.0-or-later

#include "nativefaceauthbackend.h"

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>

class NativeFaceAuthBackendTest final : public QObject
{
    Q_OBJECT

  private Q_SLOTS:
    void unavailableEngineCompletesAsynchronously();
    void availableLocalIdentityReportsSeparatedCapabilities();
    void newerGenerationCancelsOlderRefresh();
    void explicitCancellationIsGenerationAware();
    void installedRuntimeReportsRealStatus();
};

void NativeFaceAuthBackendTest::unavailableEngineCompletesAsynchronously()
{
    NativeFaceAuthBackend backend([]() { return NativeFaceAuthBackend::Availability{}; });
    QSignalSpy progressSpy(&backend, &FaceAuthBackend::refreshProgress);
    QSignalSpy completedSpy(&backend, &FaceAuthBackend::refreshCompleted);
    QElapsedTimer elapsed;
    elapsed.start();

    backend.requestRefresh(1);

    QVERIFY(elapsed.elapsed() < 100);
    QCOMPARE(progressSpy.count(), 1);
    QCOMPARE(completedSpy.count(), 0);
    QTRY_COMPARE(completedSpy.count(), 1);
    const EngineSnapshot snapshot = qvariant_cast<EngineSnapshot>(completedSpy.constFirst().at(1));
    QVERIFY(!snapshot.engineAvailable);
    QCOMPARE(snapshot.protocol.state, ResultState::Failed);
    QCOMPARE(snapshot.protocol.error->code, QStringLiteral("native-engine-unavailable"));
    QCOMPARE(snapshot.capabilities.authentication, OperationSupport::Unsupported);
    QCOMPARE(snapshot.capabilities.enrollment, OperationSupport::Unsupported);
    QCOMPARE(snapshot.capabilities.pamConfiguration, OperationSupport::Unsupported);
    QCOMPARE(snapshot.capabilities.encryptedPersistence, OperationSupport::Unsupported);
    QCOMPARE(snapshot.capabilities.localVerification, OperationSupport::Unsupported);
    QCOMPARE(snapshot.capabilities.profileDeletion, OperationSupport::Unsupported);
}

void NativeFaceAuthBackendTest::availableLocalIdentityReportsSeparatedCapabilities()
{
    NativeFaceAuthBackend backend(
        []()
        {
            NativeFaceAuthBackend::Availability availability{
                true, 2, QStringLiteral("0.1.0-local-identity"), true, true,
            };
            availability.vaultState = EngineStatusSnapshot::VaultState::Ready;
            availability.profileEnrolled = true;
            availability.sampleCount = 5;
            availability.keyProviderState = EngineStatusSnapshot::KeyProviderState::Available;
            return availability;
        });
    QSignalSpy completedSpy(&backend, &FaceAuthBackend::refreshCompleted);
    backend.requestRefresh(4);
    QTRY_COMPARE(completedSpy.count(), 1);

    const EngineSnapshot snapshot = qvariant_cast<EngineSnapshot>(completedSpy.constFirst().at(1));
    QVERIFY(snapshot.engineAvailable);
    QVERIFY(snapshot.protocolAvailable());
    QVERIFY(snapshot.capabilities.supports(EngineFeature::StatusRead));
    QVERIFY(snapshot.capabilities.supports(EngineFeature::CapabilityRead));
    QVERIFY(snapshot.capabilities.supports(EngineFeature::DetectorAnalysis));
    QVERIFY(snapshot.capabilities.supports(EngineFeature::EmbeddingExtraction));
    QVERIFY(snapshot.capabilities.supports(EngineFeature::UserSessionEnrollment));
    QVERIFY(snapshot.capabilities.supports(EngineFeature::EncryptedPersistence));
    QVERIFY(snapshot.capabilities.supports(EngineFeature::LocalVerification));
    QVERIFY(snapshot.capabilities.supports(EngineFeature::ProfileDeletion));
    QCOMPARE(snapshot.capabilities.authentication, OperationSupport::Unsupported);
    QCOMPARE(snapshot.capabilities.enrollment, OperationSupport::Supported);
    QCOMPARE(snapshot.capabilities.pamConfiguration, OperationSupport::Unsupported);
    QCOMPARE(snapshot.capabilities.encryptedPersistence, OperationSupport::Supported);
    QCOMPARE(snapshot.capabilities.localVerification, OperationSupport::Supported);
    QCOMPARE(snapshot.capabilities.profileDeletion, OperationSupport::Supported);
    QCOMPARE(snapshot.status.state, ResultState::Available);
    QVERIFY(snapshot.status.data->detectorModelAvailable);
    QVERIFY(snapshot.status.data->embeddingModelAvailable);
    QCOMPARE(snapshot.status.data->keyProviderState, EngineStatusSnapshot::KeyProviderState::Available);
    QCOMPARE(snapshot.status.data->vaultState, EngineStatusSnapshot::VaultState::Ready);
    QVERIFY(snapshot.status.data->profileEnrolled);
    QCOMPARE(snapshot.status.data->sampleCount, quint8(5));
}

void NativeFaceAuthBackendTest::newerGenerationCancelsOlderRefresh()
{
    NativeFaceAuthBackend backend;
    QSignalSpy cancelledSpy(&backend, &FaceAuthBackend::refreshCancelled);
    QSignalSpy completedSpy(&backend, &FaceAuthBackend::refreshCompleted);

    backend.requestRefresh(10);
    backend.requestRefresh(11);

    QCOMPARE(cancelledSpy.count(), 1);
    QCOMPARE(cancelledSpy.constFirst().at(0).toULongLong(), quint64(10));
    QTRY_COMPARE(completedSpy.count(), 1);
    QCOMPARE(completedSpy.constFirst().at(0).toULongLong(), quint64(11));
}

void NativeFaceAuthBackendTest::explicitCancellationIsGenerationAware()
{
    NativeFaceAuthBackend backend;
    QSignalSpy cancelledSpy(&backend, &FaceAuthBackend::refreshCancelled);
    QSignalSpy completedSpy(&backend, &FaceAuthBackend::refreshCompleted);

    backend.requestRefresh(20);
    backend.cancelRefresh();

    QCOMPARE(cancelledSpy.count(), 1);
    QCOMPARE(cancelledSpy.constFirst().at(0).toULongLong(), quint64(20));
    QTest::qWait(1);
    QCOMPARE(completedSpy.count(), 0);
}

void NativeFaceAuthBackendTest::installedRuntimeReportsRealStatus()
{
    if (!qEnvironmentVariableIsSet("KFACEAUTH_TEST_INSTALLED_RUNTIME"))
        QSKIP("requires the installed RPM runtime");

    NativeFaceAuthBackend backend;
    QSignalSpy completedSpy(&backend, &FaceAuthBackend::refreshCompleted);
    backend.requestRefresh(30);
    QTRY_COMPARE_WITH_TIMEOUT(completedSpy.count(), 1, 15000);

    const EngineSnapshot snapshot = qvariant_cast<EngineSnapshot>(completedSpy.constFirst().at(1));
    QVERIFY(snapshot.engineAvailable);
    QVERIFY(snapshot.protocolAvailable());
    QCOMPARE(snapshot.status.state, ResultState::Available);
    QVERIFY(snapshot.status.data->detectorModelAvailable);
    QVERIFY(snapshot.status.data->embeddingModelAvailable);
    QCOMPARE(snapshot.status.data->vaultState, EngineStatusSnapshot::VaultState::Absent);
    QVERIFY(!snapshot.status.data->profileEnrolled);
    QCOMPARE(snapshot.status.data->sampleCount, quint8(0));
}

QTEST_GUILESS_MAIN(NativeFaceAuthBackendTest)

#include "test_nativefaceauthbackend.moc"
