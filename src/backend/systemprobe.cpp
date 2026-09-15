// SPDX-License-Identifier: GPL-3.0-or-later

#include "systemprobe.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QRegularExpression>
#include <QtConcurrentRun>

namespace
{
constexpr qsizetype MaximumProbeOutput = 256 * 1024;

QString translate(const char *text)
{
    return QCoreApplication::translate("SystemProbe", text);
}

QByteArray readBoundedFile(const QString &path, qsizetype maximumBytes = MaximumProbeOutput)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.read(maximumBytes) : QByteArray();
}

QString displayManagerLabel(const QString &target)
{
    const QString service = QFileInfo(target).fileName().toLower();
    if (service.contains(QStringLiteral("plasmalogin")) || service.contains(QStringLiteral("plasma-login-manager")))
        return QStringLiteral("Plasma Login Manager");
    if (service.contains(QStringLiteral("sddm")))
        return QStringLiteral("SDDM");
    return service.isEmpty() ? QStringLiteral("Unknown") : QStringLiteral("Other");
}

SystemStateSnapshot::SecureBootStatus secureBootStatus(const SystemProbeInputs &inputs)
{
    if (!inputs.secureBootVariablePresent || inputs.secureBootVariable.size() < 5)
        return SystemStateSnapshot::SecureBootStatus::Unknown;
    return inputs.secureBootVariable.at(4) == '\x01' ? SystemStateSnapshot::SecureBootStatus::Enabled
                                                     : SystemStateSnapshot::SecureBootStatus::Disabled;
}

SystemStateSnapshot::CapabilityStatus capability(OperationSupport support)
{
    return support == OperationSupport::Supported ? SystemStateSnapshot::CapabilityStatus::Supported
                                                  : SystemStateSnapshot::CapabilityStatus::Unsupported;
}
} // namespace

SystemProbe::SystemProbe(QObject *parent) : QObject(parent) {}

void SystemProbe::requestProbe(quint64 generation, const EngineSnapshot &engine)
{
    m_latestGeneration = generation;
    auto *watcher = new QFutureWatcher<SystemStateSnapshot>(this);
    connect(watcher, &QFutureWatcher<SystemStateSnapshot>::finished, this,
            [this, watcher, generation]()
            {
                if (generation == m_latestGeneration)
                    Q_EMIT probeCompleted(generation, watcher->result());
                watcher->deleteLater();
            });
    watcher->setFuture(QtConcurrent::run([engine]() { return SystemProbe().probe(engine); }));
}

SystemStateSnapshot SystemProbe::probe(const EngineSnapshot &engine) const
{
    SystemProbeInputs inputs;
    inputs.osRelease = readBoundedFile(QStringLiteral("/etc/os-release"));
    inputs.plasmaVersion.clear();
    const QFileInfo displayManager(QStringLiteral("/etc/systemd/system/display-manager.service"));
    inputs.displayManagerTarget =
        displayManager.isSymLink() ? displayManager.symLinkTarget() : displayManager.canonicalFilePath();

    const QDir efivars(QStringLiteral("/sys/firmware/efi/efivars"));
    const QStringList variables =
        efivars.entryList({QStringLiteral("SecureBoot-*")}, QDir::Files | QDir::Readable, QDir::Name);
    if (!variables.isEmpty())
    {
        inputs.secureBootVariablePresent = true;
        inputs.secureBootVariable = readBoundedFile(efivars.filePath(variables.constFirst()), 8);
    }
    inputs.engine = engine;
    return evaluate(inputs);
}

SystemStateSnapshot SystemProbe::evaluate(const SystemProbeInputs &inputs)
{
    SystemStateSnapshot state;
    state.scenarioId = QStringLiteral("native-local-identity-mvp");
    state.dataSource = translate("Live local system");
    state.liveData = true;
    state.distribution = parseOsReleaseValue(inputs.osRelease, QStringLiteral("ID"));
    state.fedoraVersion = parseOsReleaseValue(inputs.osRelease, QStringLiteral("VERSION_ID"));
    state.plasmaVersion = inputs.plasmaVersion;
    state.activeDisplayManager = displayManagerLabel(inputs.displayManagerTarget);
    state.secureBootStatus = secureBootStatus(inputs);
    state.engineVersion = inputs.engine.engineVersion();
    state.visionStatus = capability(inputs.engine.capabilities.detectorAnalysis);
    state.enrollmentStatus = capability(inputs.engine.capabilities.enrollment);
    state.authenticationStatus = capability(inputs.engine.capabilities.authentication);
    state.pamStatus = capability(inputs.engine.capabilities.pamConfiguration);
    state.templatePersistenceStatus = capability(inputs.engine.capabilities.encryptedPersistence);

    if (inputs.engine.status.data)
    {
        const auto &status = *inputs.engine.status.data;
        state.modelStatus = status.detectorModelAvailable && status.embeddingModelAvailable
                                ? SystemStateSnapshot::ModelStatus::Verified
                                : SystemStateSnapshot::ModelStatus::Unavailable;
        state.keyProviderStatus = status.keyProviderState == EngineStatusSnapshot::KeyProviderState::Available
                                      ? SystemStateSnapshot::KeyProviderStatus::Available
                                      : (status.keyProviderState == EngineStatusSnapshot::KeyProviderState::Locked
                                             ? SystemStateSnapshot::KeyProviderStatus::Locked
                                             : SystemStateSnapshot::KeyProviderStatus::Unavailable);
        state.vaultStatus = status.vaultState == EngineStatusSnapshot::VaultState::Absent
                                ? SystemStateSnapshot::VaultStatus::Absent
                                : (status.vaultState == EngineStatusSnapshot::VaultState::Ready
                                       ? SystemStateSnapshot::VaultStatus::Ready
                                       : (status.vaultState == EngineStatusSnapshot::VaultState::Corrupt
                                              ? SystemStateSnapshot::VaultStatus::Unreadable
                                              : (status.vaultState == EngineStatusSnapshot::VaultState::ModelMismatch
                                                     ? SystemStateSnapshot::VaultStatus::ModelMismatch
                                                     : SystemStateSnapshot::VaultStatus::Unknown)));
        state.profileEnrolled = status.profileEnrolled;
        state.profileSampleCount = status.sampleCount;
    }

    if (state.distribution.compare(QStringLiteral("fedora"), Qt::CaseInsensitive) != 0 ||
        state.fedoraVersion != QStringLiteral("44"))
    {
        state.headline = translate("This system is not qualified");
        state.summary =
            translate("LoofiFace-ID is qualified only for Fedora 44 with KDE Plasma. Detected system: %1 %2.")
                .arg(state.distribution.isEmpty() ? translate("unknown distribution") : state.distribution,
                     state.fedoraVersion.isEmpty() ? translate("unknown version") : state.fedoraVersion);
        state.issueCode = QStringLiteral("unsupported-platform");
        state.engineStatus = inputs.engine.engineAvailable ? SystemStateSnapshot::EngineStatus::LocalIdentityAvailable
                                                           : SystemStateSnapshot::EngineStatus::Unavailable;
        return state;
    }

    if (!inputs.engine.engineAvailable)
    {
        state.headline = translate("Native engine unavailable");
        state.summary =
            translate("The KCM remains usable for camera checks. Local identity components are unavailable; PAM "
                      "and system authentication remain unsupported.");
        state.issueCode = QStringLiteral("native-engine-unavailable");
        state.engineStatus = SystemStateSnapshot::EngineStatus::Unavailable;
        return state;
    }

    if (!inputs.engine.protocolAvailable())
    {
        state.headline = translate("Native protocol unavailable");
        state.summary = translate("The local engine did not provide the versioned local-identity status protocol.");
        state.issueCode = QStringLiteral("native-protocol-unavailable");
        state.engineStatus = SystemStateSnapshot::EngineStatus::ProtocolError;
        return state;
    }

    if (!inputs.engine.status.data)
    {
        state.headline = translate("Local identity worker needs attention");
        state.summary =
            translate("The local worker status could not be read. Retry diagnostics before testing a profile.");
        state.issueCode = QStringLiteral("identity-worker-unavailable");
        state.engineStatus = SystemStateSnapshot::EngineStatus::LocalIdentityAvailable;
        return state;
    }

    if (state.modelStatus == SystemStateSnapshot::ModelStatus::Unavailable)
    {
        state.headline = translate("Verified models need attention");
        state.summary = translate("The required local model inventory is incomplete or unavailable.");
        state.issueCode = QStringLiteral("model-unavailable");
        state.engineStatus = SystemStateSnapshot::EngineStatus::LocalIdentityAvailable;
        return state;
    }

    if (state.keyProviderStatus == SystemStateSnapshot::KeyProviderStatus::Unavailable)
    {
        state.headline = translate("KWallet is unavailable");
        state.summary = translate("KFaceAuth cannot create or read an encrypted profile until KWallet is available.");
        state.issueCode = QStringLiteral("kwallet-unavailable");
        state.engineStatus = SystemStateSnapshot::EngineStatus::LocalIdentityAvailable;
        return state;
    }

    if (state.keyProviderStatus == SystemStateSnapshot::KeyProviderStatus::Locked)
    {
        state.headline = translate("KWallet needs attention");
        state.summary = translate("Unlock KWallet in the current session, then refresh before using the profile.");
        state.issueCode = QStringLiteral("kwallet-locked");
        state.engineStatus = SystemStateSnapshot::EngineStatus::LocalIdentityAvailable;
        return state;
    }

    if (state.vaultStatus == SystemStateSnapshot::VaultStatus::Unknown)
    {
        state.headline = translate("Encrypted profile status unavailable");
        state.summary = translate("The local profile status could not be read. Retry Diagnostics before testing.");
        state.issueCode = QStringLiteral("vault-unavailable");
        state.engineStatus = SystemStateSnapshot::EngineStatus::LocalIdentityAvailable;
        return state;
    }

    if (state.vaultStatus == SystemStateSnapshot::VaultStatus::Unreadable)
    {
        state.headline = translate("The encrypted profile needs attention");
        state.summary = translate(
            "The existing profile is unreadable. Use the explicit reset action only if you accept re-enrollment.");
        state.issueCode = QStringLiteral("vault-unreadable");
        state.engineStatus = SystemStateSnapshot::EngineStatus::LocalIdentityAvailable;
        return state;
    }

    if (state.vaultStatus == SystemStateSnapshot::VaultStatus::ModelMismatch)
    {
        state.headline = translate("The profile model has changed");
        state.summary = translate("The encrypted profile was created for a different verified model version.");
        state.issueCode = QStringLiteral("vault-model-mismatch");
        state.engineStatus = SystemStateSnapshot::EngineStatus::LocalIdentityAvailable;
        return state;
    }

    state.headline = translate("Local identity engine available");
    state.summary =
        translate("Verified detector and embedding models support encrypted user-session enrollment and "
                  "explicit local comparison. PAM, liveness, and system authentication remain unsupported.");
    state.engineStatus = SystemStateSnapshot::EngineStatus::LocalIdentityAvailable;
    return state;
}

QString SystemProbe::parseOsReleaseValue(const QByteArray &contents, const QString &key)
{
    for (const QByteArray &line : contents.split('\n'))
    {
        const int separator = line.indexOf('=');
        if (separator <= 0 || QString::fromLatin1(line.left(separator)) != key)
            continue;
        QString value = QString::fromUtf8(line.mid(separator + 1)).trimmed();
        if (value.size() >= 2 && ((value.startsWith(QLatin1Char('"')) && value.endsWith(QLatin1Char('"'))) ||
                                  (value.startsWith(QLatin1Char('\'')) && value.endsWith(QLatin1Char('\'')))))
            value = value.mid(1, value.size() - 2);
        static const QRegularExpression safeValue(QStringLiteral(R"(\A[A-Za-z0-9._+~ -]{1,96}\z)"));
        return safeValue.match(value).hasMatch() ? value : QString();
    }
    return {};
}
