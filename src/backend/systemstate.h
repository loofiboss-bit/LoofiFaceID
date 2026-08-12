// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QString>

struct SystemStateSnapshot
{
    enum class EngineStatus
    {
        LocalIdentityAvailable,
        Unavailable,
        ProtocolError,
    };

    enum class CapabilityStatus
    {
        Supported,
        Unsupported,
        Unknown,
    };

    enum class SecureBootStatus
    {
        Enabled,
        Disabled,
        Unknown,
    };

    enum class ModelStatus
    {
        Verified,
        Unavailable,
        Unknown,
    };

    enum class KeyProviderStatus
    {
        Available,
        Locked,
        Unavailable,
        Unknown,
    };

    enum class VaultStatus
    {
        Unknown,
        Absent,
        Ready,
        Unreadable,
        ModelMismatch,
    };

    QString scenarioId;
    QString headline;
    QString summary;
    QString issueCode;
    QString dataSource;
    QString fedoraVersion;
    QString plasmaVersion;
    QString engineVersion;
    QString activeDisplayManager;
    EngineStatus engineStatus = EngineStatus::Unavailable;
    CapabilityStatus visionStatus = CapabilityStatus::Unsupported;
    CapabilityStatus enrollmentStatus = CapabilityStatus::Unsupported;
    CapabilityStatus authenticationStatus = CapabilityStatus::Unsupported;
    CapabilityStatus pamStatus = CapabilityStatus::Unsupported;
    CapabilityStatus templatePersistenceStatus = CapabilityStatus::Unsupported;
    SecureBootStatus secureBootStatus = SecureBootStatus::Unknown;
    ModelStatus modelStatus = ModelStatus::Unknown;
    KeyProviderStatus keyProviderStatus = KeyProviderStatus::Unknown;
    VaultStatus vaultStatus = VaultStatus::Unknown;
    bool profileEnrolled = false;
    int profileSampleCount = 0;
    bool liveData = false;
};

class SystemState final : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString scenarioId READ scenarioId NOTIFY stateChanged)
    Q_PROPERTY(QString headline READ headline NOTIFY stateChanged)
    Q_PROPERTY(QString summary READ summary NOTIFY stateChanged)
    Q_PROPERTY(QString issueCode READ issueCode NOTIFY stateChanged)
    Q_PROPERTY(QString dataSource READ dataSource NOTIFY stateChanged)
    Q_PROPERTY(QString fedoraVersion READ fedoraVersion NOTIFY stateChanged)
    Q_PROPERTY(QString plasmaVersion READ plasmaVersion NOTIFY stateChanged)
    Q_PROPERTY(QString engineVersion READ engineVersion NOTIFY stateChanged)
    Q_PROPERTY(QString activeDisplayManager READ activeDisplayManager NOTIFY stateChanged)
    Q_PROPERTY(EngineStatus engineStatus READ engineStatus NOTIFY stateChanged)
    Q_PROPERTY(CapabilityStatus visionStatus READ visionStatus NOTIFY stateChanged)
    Q_PROPERTY(CapabilityStatus enrollmentStatus READ enrollmentStatus NOTIFY stateChanged)
    Q_PROPERTY(CapabilityStatus authenticationStatus READ authenticationStatus NOTIFY stateChanged)
    Q_PROPERTY(CapabilityStatus pamStatus READ pamStatus NOTIFY stateChanged)
    Q_PROPERTY(CapabilityStatus templatePersistenceStatus READ templatePersistenceStatus NOTIFY stateChanged)
    Q_PROPERTY(SecureBootStatus secureBootStatus READ secureBootStatus NOTIFY stateChanged)
    Q_PROPERTY(ModelStatus modelStatus READ modelStatus NOTIFY stateChanged)
    Q_PROPERTY(KeyProviderStatus keyProviderStatus READ keyProviderStatus NOTIFY stateChanged)
    Q_PROPERTY(VaultStatus vaultStatus READ vaultStatus NOTIFY stateChanged)
    Q_PROPERTY(bool profileEnrolled READ profileEnrolled NOTIFY stateChanged)
    Q_PROPERTY(int profileSampleCount READ profileSampleCount NOTIFY stateChanged)
    Q_PROPERTY(bool liveData READ liveData NOTIFY stateChanged)
    Q_PROPERTY(bool engineReady READ engineReady NOTIFY stateChanged)
    Q_PROPERTY(bool modelsVerified READ modelsVerified NOTIFY stateChanged)
    Q_PROPERTY(bool keyAvailable READ keyAvailable NOTIFY stateChanged)
    Q_PROPERTY(bool vaultReady READ vaultReady NOTIFY stateChanged)
    Q_PROPERTY(QString engineStatusLabel READ engineStatusLabel NOTIFY stateChanged)
    Q_PROPERTY(QString visionStatusLabel READ visionStatusLabel NOTIFY stateChanged)
    Q_PROPERTY(QString enrollmentStatusLabel READ enrollmentStatusLabel NOTIFY stateChanged)
    Q_PROPERTY(QString authenticationStatusLabel READ authenticationStatusLabel NOTIFY stateChanged)
    Q_PROPERTY(QString pamStatusLabel READ pamStatusLabel NOTIFY stateChanged)
    Q_PROPERTY(QString templatePersistenceStatusLabel READ templatePersistenceStatusLabel NOTIFY stateChanged)
    Q_PROPERTY(QString secureBootStatusLabel READ secureBootStatusLabel NOTIFY stateChanged)
    Q_PROPERTY(QString modelStatusLabel READ modelStatusLabel NOTIFY stateChanged)
    Q_PROPERTY(QString keyProviderStatusLabel READ keyProviderStatusLabel NOTIFY stateChanged)
    Q_PROPERTY(QString vaultStatusLabel READ vaultStatusLabel NOTIFY stateChanged)

  public:
    enum class EngineStatus
    {
        LocalIdentityAvailable,
        Unavailable,
        ProtocolError,
    };
    Q_ENUM(EngineStatus)

    enum class CapabilityStatus
    {
        Supported,
        Unsupported,
        Unknown,
    };
    Q_ENUM(CapabilityStatus)

    enum class SecureBootStatus
    {
        Enabled,
        Disabled,
        Unknown,
    };
    Q_ENUM(SecureBootStatus)

    enum class ModelStatus
    {
        Verified,
        Unavailable,
        Unknown,
    };
    Q_ENUM(ModelStatus)

    enum class KeyProviderStatus
    {
        Available,
        Locked,
        Unavailable,
        Unknown,
    };
    Q_ENUM(KeyProviderStatus)

    enum class VaultStatus
    {
        Unknown,
        Absent,
        Ready,
        Unreadable,
        ModelMismatch,
    };
    Q_ENUM(VaultStatus)

    explicit SystemState(QObject *parent = nullptr);
    void apply(const SystemStateSnapshot &snapshot);

    [[nodiscard]] QString scenarioId() const;
    [[nodiscard]] QString headline() const;
    [[nodiscard]] QString summary() const;
    [[nodiscard]] QString issueCode() const;
    [[nodiscard]] QString dataSource() const;
    [[nodiscard]] QString fedoraVersion() const;
    [[nodiscard]] QString plasmaVersion() const;
    [[nodiscard]] QString engineVersion() const;
    [[nodiscard]] QString activeDisplayManager() const;
    [[nodiscard]] EngineStatus engineStatus() const;
    [[nodiscard]] CapabilityStatus visionStatus() const;
    [[nodiscard]] CapabilityStatus enrollmentStatus() const;
    [[nodiscard]] CapabilityStatus authenticationStatus() const;
    [[nodiscard]] CapabilityStatus pamStatus() const;
    [[nodiscard]] CapabilityStatus templatePersistenceStatus() const;
    [[nodiscard]] SecureBootStatus secureBootStatus() const;
    [[nodiscard]] ModelStatus modelStatus() const;
    [[nodiscard]] KeyProviderStatus keyProviderStatus() const;
    [[nodiscard]] VaultStatus vaultStatus() const;
    [[nodiscard]] bool profileEnrolled() const;
    [[nodiscard]] int profileSampleCount() const;
    [[nodiscard]] bool liveData() const;
    [[nodiscard]] bool engineReady() const;
    [[nodiscard]] bool modelsVerified() const;
    [[nodiscard]] bool keyAvailable() const;
    [[nodiscard]] bool vaultReady() const;

    [[nodiscard]] QString engineStatusLabel() const;
    [[nodiscard]] QString visionStatusLabel() const;
    [[nodiscard]] QString enrollmentStatusLabel() const;
    [[nodiscard]] QString authenticationStatusLabel() const;
    [[nodiscard]] QString pamStatusLabel() const;
    [[nodiscard]] QString templatePersistenceStatusLabel() const;
    [[nodiscard]] QString secureBootStatusLabel() const;
    [[nodiscard]] QString modelStatusLabel() const;
    [[nodiscard]] QString keyProviderStatusLabel() const;
    [[nodiscard]] QString vaultStatusLabel() const;

  Q_SIGNALS:
    void stateChanged();

  private:
    [[nodiscard]] static QString capabilityLabel(SystemStateSnapshot::CapabilityStatus status);
    SystemStateSnapshot m_snapshot;
};

Q_DECLARE_METATYPE(SystemStateSnapshot)
