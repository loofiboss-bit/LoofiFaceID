// SPDX-License-Identifier: GPL-3.0-or-later

#include "systemstate.h"

SystemState::SystemState(QObject *parent) : QObject(parent) {}

void SystemState::apply(const SystemStateSnapshot &snapshot)
{
    m_snapshot = snapshot;
    Q_EMIT stateChanged();
}

QString SystemState::scenarioId() const
{
    return m_snapshot.scenarioId;
}

QString SystemState::headline() const
{
    return m_snapshot.headline;
}

QString SystemState::summary() const
{
    return m_snapshot.summary;
}

QString SystemState::issueCode() const
{
    return m_snapshot.issueCode;
}

QString SystemState::dataSource() const
{
    return m_snapshot.dataSource;
}

QString SystemState::distribution() const
{
    return m_snapshot.distribution;
}

QString SystemState::fedoraVersion() const
{
    return m_snapshot.fedoraVersion;
}

QString SystemState::plasmaVersion() const
{
    return m_snapshot.plasmaVersion;
}

QString SystemState::engineVersion() const
{
    return m_snapshot.engineVersion;
}

QString SystemState::activeDisplayManager() const
{
    return m_snapshot.activeDisplayManager;
}

SystemState::EngineStatus SystemState::engineStatus() const
{
    return static_cast<EngineStatus>(m_snapshot.engineStatus);
}

SystemState::CapabilityStatus SystemState::visionStatus() const
{
    return static_cast<CapabilityStatus>(m_snapshot.visionStatus);
}

SystemState::CapabilityStatus SystemState::enrollmentStatus() const
{
    return static_cast<CapabilityStatus>(m_snapshot.enrollmentStatus);
}

SystemState::CapabilityStatus SystemState::authenticationStatus() const
{
    return static_cast<CapabilityStatus>(m_snapshot.authenticationStatus);
}

SystemState::CapabilityStatus SystemState::pamStatus() const
{
    return static_cast<CapabilityStatus>(m_snapshot.pamStatus);
}

SystemState::CapabilityStatus SystemState::templatePersistenceStatus() const
{
    return static_cast<CapabilityStatus>(m_snapshot.templatePersistenceStatus);
}

SystemState::SecureBootStatus SystemState::secureBootStatus() const
{
    return static_cast<SecureBootStatus>(m_snapshot.secureBootStatus);
}

SystemState::ModelStatus SystemState::modelStatus() const
{
    return static_cast<ModelStatus>(m_snapshot.modelStatus);
}

SystemState::KeyProviderStatus SystemState::keyProviderStatus() const
{
    return static_cast<KeyProviderStatus>(m_snapshot.keyProviderStatus);
}

SystemState::VaultStatus SystemState::vaultStatus() const
{
    return static_cast<VaultStatus>(m_snapshot.vaultStatus);
}

bool SystemState::profileEnrolled() const
{
    return m_snapshot.profileEnrolled;
}

int SystemState::profileSampleCount() const
{
    return m_snapshot.profileSampleCount;
}

bool SystemState::liveData() const
{
    return m_snapshot.liveData;
}

bool SystemState::engineReady() const
{
    return m_snapshot.engineStatus == SystemStateSnapshot::EngineStatus::LocalIdentityAvailable;
}

bool SystemState::modelsVerified() const
{
    return m_snapshot.modelStatus == SystemStateSnapshot::ModelStatus::Verified;
}

bool SystemState::keyAvailable() const
{
    return m_snapshot.keyProviderStatus == SystemStateSnapshot::KeyProviderStatus::Available;
}

bool SystemState::vaultReady() const
{
    return m_snapshot.vaultStatus == SystemStateSnapshot::VaultStatus::Ready;
}

QString SystemState::engineStatusLabel() const
{
    switch (m_snapshot.engineStatus)
    {
    case SystemStateSnapshot::EngineStatus::LocalIdentityAvailable:
        return tr("Local identity available");
    case SystemStateSnapshot::EngineStatus::Unavailable:
        return tr("Unavailable");
    case SystemStateSnapshot::EngineStatus::ProtocolError:
        return tr("Protocol error");
    }
    return tr("Unavailable");
}

QString SystemState::capabilityLabel(SystemStateSnapshot::CapabilityStatus status)
{
    switch (status)
    {
    case SystemStateSnapshot::CapabilityStatus::Supported:
        return tr("Supported");
    case SystemStateSnapshot::CapabilityStatus::Unsupported:
        return tr("Not implemented");
    case SystemStateSnapshot::CapabilityStatus::Unknown:
        return tr("Unknown");
    }
    return tr("Unknown");
}

QString SystemState::visionStatusLabel() const
{
    return capabilityLabel(m_snapshot.visionStatus);
}

QString SystemState::enrollmentStatusLabel() const
{
    return capabilityLabel(m_snapshot.enrollmentStatus);
}

QString SystemState::authenticationStatusLabel() const
{
    return capabilityLabel(m_snapshot.authenticationStatus);
}

QString SystemState::pamStatusLabel() const
{
    return capabilityLabel(m_snapshot.pamStatus);
}

QString SystemState::templatePersistenceStatusLabel() const
{
    return capabilityLabel(m_snapshot.templatePersistenceStatus);
}

QString SystemState::secureBootStatusLabel() const
{
    switch (m_snapshot.secureBootStatus)
    {
    case SystemStateSnapshot::SecureBootStatus::Enabled:
        return tr("Enabled");
    case SystemStateSnapshot::SecureBootStatus::Disabled:
        return tr("Disabled");
    case SystemStateSnapshot::SecureBootStatus::Unknown:
        return tr("Unknown");
    }
    return tr("Unknown");
}

QString SystemState::modelStatusLabel() const
{
    switch (m_snapshot.modelStatus)
    {
    case SystemStateSnapshot::ModelStatus::Verified:
        return tr("Verified");
    case SystemStateSnapshot::ModelStatus::Unavailable:
        return tr("Unavailable");
    case SystemStateSnapshot::ModelStatus::Unknown:
        return tr("Unknown");
    }
    return tr("Unknown");
}

QString SystemState::keyProviderStatusLabel() const
{
    switch (m_snapshot.keyProviderStatus)
    {
    case SystemStateSnapshot::KeyProviderStatus::Available:
        return tr("Available");
    case SystemStateSnapshot::KeyProviderStatus::Locked:
        return tr("Locked");
    case SystemStateSnapshot::KeyProviderStatus::Unavailable:
        return tr("Unavailable");
    case SystemStateSnapshot::KeyProviderStatus::Unknown:
        return tr("Unknown");
    }
    return tr("Unknown");
}

QString SystemState::vaultStatusLabel() const
{
    switch (m_snapshot.vaultStatus)
    {
    case SystemStateSnapshot::VaultStatus::Absent:
        return tr("No profile");
    case SystemStateSnapshot::VaultStatus::Ready:
        return tr("Ready");
    case SystemStateSnapshot::VaultStatus::Unreadable:
        return tr("Unreadable");
    case SystemStateSnapshot::VaultStatus::ModelMismatch:
        return tr("Model mismatch");
    case SystemStateSnapshot::VaultStatus::Unknown:
        return tr("Unknown");
    }
    return tr("Unknown");
}
