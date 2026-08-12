// SPDX-License-Identifier: GPL-3.0-or-later

#include "systemprobe.h"
#include "systemstate.h"

#include <QSignalSpy>
#include <QTest>

namespace
{
SystemProbeInputs baseInputs()
{
    SystemProbeInputs inputs;
    inputs.osRelease = "NAME=Fedora Linux\nID=fedora\nVERSION_ID=\"44\"\n";
    inputs.plasmaVersion = QStringLiteral("6.6.2");
    inputs.displayManagerTarget = QStringLiteral("/usr/lib/systemd/system/plasmalogin.service");
    inputs.secureBootVariablePresent = true;
    inputs.secureBootVariable = QByteArray::fromHex("0700000001");
    return inputs;
}
} // namespace

class SystemStateTest final : public QObject
{
    Q_OBJECT

  private Q_SLOTS:
    void unavailableEngineFailsClosed();
    void localIdentityReportsSupportedAndUnsupportedOperations();
    void degradedIdentityReportsTypedKWalletAndVaultIssues();
    void applyingStateNotifiesConsumers();
    void hostProbeRunsAsynchronously();
};

void SystemStateTest::unavailableEngineFailsClosed()
{
    const SystemStateSnapshot snapshot = SystemProbe::evaluate(baseInputs());

    QCOMPARE(snapshot.engineStatus, SystemStateSnapshot::EngineStatus::Unavailable);
    QCOMPARE(snapshot.issueCode, QStringLiteral("native-engine-unavailable"));
    QCOMPARE(snapshot.authenticationStatus, SystemStateSnapshot::CapabilityStatus::Unsupported);
    QCOMPARE(snapshot.enrollmentStatus, SystemStateSnapshot::CapabilityStatus::Unsupported);
    QCOMPARE(snapshot.pamStatus, SystemStateSnapshot::CapabilityStatus::Unsupported);
    QCOMPARE(snapshot.templatePersistenceStatus, SystemStateSnapshot::CapabilityStatus::Unsupported);
}

void SystemStateTest::localIdentityReportsSupportedAndUnsupportedOperations()
{
    SystemProbeInputs inputs = baseInputs();
    inputs.engine.engineAvailable = true;
    inputs.engine.protocol = EngineProtocolSnapshot{2, QStringLiteral("0.1.0-local-identity")};
    inputs.engine.status = EngineStatusSnapshot{EngineStatusSnapshot::State::Ready};
    inputs.engine.capabilities.detectorAnalysis = OperationSupport::Supported;
    inputs.engine.capabilities.enrollment = OperationSupport::Supported;
    inputs.engine.capabilities.encryptedPersistence = OperationSupport::Supported;
    inputs.engine.status.data->detectorModelAvailable = true;
    inputs.engine.status.data->embeddingModelAvailable = true;
    inputs.engine.status.data->keyProviderState = EngineStatusSnapshot::KeyProviderState::Available;
    inputs.engine.status.data->vaultState = EngineStatusSnapshot::VaultState::Absent;

    const SystemStateSnapshot snapshot = SystemProbe::evaluate(inputs);

    QCOMPARE(snapshot.engineStatus, SystemStateSnapshot::EngineStatus::LocalIdentityAvailable);
    QCOMPARE(snapshot.engineVersion, QStringLiteral("0.1.0-local-identity"));
    QCOMPARE(snapshot.visionStatus, SystemStateSnapshot::CapabilityStatus::Supported);
    QCOMPARE(snapshot.enrollmentStatus, SystemStateSnapshot::CapabilityStatus::Supported);
    QCOMPARE(snapshot.templatePersistenceStatus, SystemStateSnapshot::CapabilityStatus::Supported);
    QCOMPARE(snapshot.modelStatus, SystemStateSnapshot::ModelStatus::Verified);
    QCOMPARE(snapshot.keyProviderStatus, SystemStateSnapshot::KeyProviderStatus::Available);
    QCOMPARE(snapshot.vaultStatus, SystemStateSnapshot::VaultStatus::Absent);
    QCOMPARE(snapshot.authenticationStatus, SystemStateSnapshot::CapabilityStatus::Unsupported);
    QCOMPARE(snapshot.pamStatus, SystemStateSnapshot::CapabilityStatus::Unsupported);
    QVERIFY(snapshot.issueCode.isEmpty());
}

void SystemStateTest::degradedIdentityReportsTypedKWalletAndVaultIssues()
{
    SystemProbeInputs inputs = baseInputs();
    inputs.engine.engineAvailable = true;
    inputs.engine.protocol = EngineProtocolSnapshot{2, QStringLiteral("0.1.0-local-identity")};
    inputs.engine.status = EngineStatusSnapshot{EngineStatusSnapshot::State::Degraded};
    inputs.engine.status.data->detectorModelAvailable = true;
    inputs.engine.status.data->embeddingModelAvailable = true;
    inputs.engine.status.data->keyProviderState = EngineStatusSnapshot::KeyProviderState::Locked;
    inputs.engine.status.data->vaultState = EngineStatusSnapshot::VaultState::Unknown;

    const SystemStateSnapshot walletSnapshot = SystemProbe::evaluate(inputs);
    QCOMPARE(walletSnapshot.issueCode, QStringLiteral("kwallet-locked"));
    QCOMPARE(walletSnapshot.keyProviderStatus, SystemStateSnapshot::KeyProviderStatus::Locked);

    inputs.engine.status.data->keyProviderState = EngineStatusSnapshot::KeyProviderState::Available;
    inputs.engine.status.data->vaultState = EngineStatusSnapshot::VaultState::ModelMismatch;
    const SystemStateSnapshot vaultSnapshot = SystemProbe::evaluate(inputs);
    QCOMPARE(vaultSnapshot.issueCode, QStringLiteral("vault-model-mismatch"));
    QCOMPARE(vaultSnapshot.vaultStatus, SystemStateSnapshot::VaultStatus::ModelMismatch);
}

void SystemStateTest::applyingStateNotifiesConsumers()
{
    SystemState state;
    QSignalSpy spy(&state, &SystemState::stateChanged);
    SystemStateSnapshot snapshot;
    snapshot.scenarioId = QStringLiteral("native-local-identity-mvp");
    snapshot.engineStatus = SystemStateSnapshot::EngineStatus::Unavailable;
    state.apply(snapshot);

    QCOMPARE(spy.count(), 1);
    QCOMPARE(state.scenarioId(), QStringLiteral("native-local-identity-mvp"));
    QCOMPARE(state.engineStatus(), SystemState::EngineStatus::Unavailable);
    QCOMPARE(state.authenticationStatus(), SystemState::CapabilityStatus::Unsupported);
    QVERIFY(!state.authenticationStatusLabel().isEmpty());
    QVERIFY(!state.modelStatusLabel().isEmpty());
    QVERIFY(!state.keyProviderStatusLabel().isEmpty());
    QVERIFY(!state.vaultStatusLabel().isEmpty());
}

void SystemStateTest::hostProbeRunsAsynchronously()
{
    SystemProbe probe;
    QSignalSpy spy(&probe, &SystemProbe::probeCompleted);
    probe.requestProbe(7, EngineSnapshot{});

    QTRY_COMPARE(spy.count(), 1);
    QCOMPARE(spy.constFirst().at(0).toULongLong(), quint64(7));
    const auto snapshot = qvariant_cast<SystemStateSnapshot>(spy.constFirst().at(1));
    QVERIFY(snapshot.liveData);
}

QTEST_GUILESS_MAIN(SystemStateTest)

#include "test_systemstate.moc"
