// SPDX-License-Identifier: GPL-3.0-or-later

#include "enrollmentsession.h"

#include "authcomponents.h"
#include "camerapreviewsession.h"
#include "identityprotocol.h"
#include "identityworkerclient.h"
#include "kwalletkeyprovider.h"
#include "pamconfiguration.h"
#include "sddmthemeconfig.h"
#include "systemauthprotocol.h"

#include <KLocalizedString>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <endian.h>
#include <fcntl.h>
#include <memory>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace
{
QString translate(const char *text)
{
    return i18nd("kcm_kfaceauth", text);
}

bool writeAll(int fd, const uint8_t *data, size_t size)
{
    size_t offset = 0;
    while (offset < size)
    {
        const ssize_t count = ::send(fd, data + offset, size - offset, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        offset += static_cast<size_t>(count);
    }
    return true;
}

bool readAll(int fd, uint8_t *data, size_t size)
{
    size_t offset = 0;
    while (offset < size)
    {
        const ssize_t count = ::recv(fd, data + offset, size - offset, 0);
        if (count < 0 && errno == EINTR)
            continue;
        if (count <= 0)
            return false;
        offset += static_cast<size_t>(count);
    }
    return true;
}

QString parseAuthPolicyMode(const QByteArray &output, bool successful)
{
    if (!successful)
        return QStringLiteral("unknown");
    for (const char *mode : {"off", "manual", "on-activity"})
        if (output == QByteArray("mode=") + mode + '\n')
            return QString::fromLatin1(mode);
    return QStringLiteral("unknown");
}

using Readiness = EnrollmentSession::Readiness;

QString readinessCode(Readiness state)
{
    switch (state)
    {
    case Readiness::Available:
        return QStringLiteral("available");
    case Readiness::Missing:
        return QStringLiteral("missing");
    case Readiness::Incompatible:
        return QStringLiteral("incompatible");
    case Readiness::Unknown:
        return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

QString readinessText(Readiness state)
{
    switch (state)
    {
    case Readiness::Available:
        return translate("Available");
    case Readiness::Missing:
        return translate("Missing");
    case Readiness::Incompatible:
        return translate("Incompatible");
    case Readiness::Unknown:
        return translate("Unknown");
    }
    return translate("Unknown");
}

Readiness markerReadiness(const QString &path, const std::function<bool(const QByteArray &)> &validate)
{
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return errno == ENOENT ? Readiness::Missing : Readiness::Unknown;
    struct stat metadata{};
    if (::fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode))
    {
        ::close(fd);
        return Readiness::Unknown;
    }
    QFile file;
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle))
    {
        ::close(fd);
        return Readiness::Unknown;
    }
    const QByteArray contents = file.read(65537);
    if (file.error() != QFileDevice::NoError)
        return Readiness::Unknown;
    if (contents.size() > 65536)
        return Readiness::Incompatible;
    return validate(contents) ? Readiness::Available : Readiness::Incompatible;
}

bool markerContainsLines(const QByteArray &contents)
{
    const QList<QByteArray> lines = contents.split('\n');
    for (const QByteArray &required :
         {QByteArray("format=org.loofifaceid.sddm-face-auth"), QByteArray("api=1"), QByteArray("sddm_version=0.21.0"),
          QByteArray("upstream_commit=63780fcd79f1dbf81a30eef48c28c699ab15aded")})
        if (std::count(lines.begin(), lines.end(), required) != 1)
            return false;
    return true;
}

bool kscreenLockerMarkerSupportsFaceAuthentication(const QByteArray &contents)
{
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(contents, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return false;
    const QJsonObject root = document.object();
    const QJsonObject themeContract = root.value(QStringLiteral("theme_contract")).toObject();
    return root.value(QStringLiteral("schema_version")).toInt() == 1 &&
           root.value(QStringLiteral("component_id")).toString() ==
               QLatin1String("org.loofi.kfaceauth.kscreenlocker") &&
           root.value(QStringLiteral("kscreenlocker_version")).toString() == QLatin1String("6.7.5") &&
           themeContract.value(QStringLiteral("interface_version")).toInt() == 1 &&
           themeContract.value(QStringLiteral("component_url")).toString() ==
               QLatin1String("qrc:/fallbacktheme/FaceAuthenticationControl.qml") &&
           themeContract.value(QStringLiteral("runtime_registration_required")).toBool();
}

// Hash only the fixed current-user ciphertext. Never expose this value to QML.
QByteArray localProfileDigest()
{
    const QString dataHome = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (!QDir::isAbsolutePath(dataHome) || QDir::cleanPath(dataHome) != dataHome)
        return {};
    int directory = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0)
        return {};
    const auto components = QFile::encodeName(dataHome).split('/');
    for (const QByteArray &component : components)
    {
        if (component.isEmpty())
            continue;
        const int child = ::openat(directory, component.constData(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        ::close(directory);
        directory = child;
        if (directory < 0)
            return {};
    }
    const int profileDirectory = ::openat(directory, "kfaceauth", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    ::close(directory);
    if (profileDirectory < 0)
        return {};
    struct stat directoryMetadata{};
    if (::fstat(profileDirectory, &directoryMetadata) != 0 || directoryMetadata.st_uid != getuid() ||
        (directoryMetadata.st_mode & 0777) != 0700)
    {
        ::close(profileDirectory);
        return {};
    }
    const int fd = ::openat(profileDirectory, "identity.vault", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    struct stat before{};
    if (fd < 0 || ::fstat(fd, &before) != 0 || !S_ISREG(before.st_mode) || before.st_uid != getuid() ||
        (before.st_mode & 0777) != 0600 || before.st_nlink != 1 || before.st_size <= 0 || before.st_size > 16384)
    {
        if (fd >= 0)
            ::close(fd);
        ::close(profileDirectory);
        return {};
    }
    QFile file;
    if (!file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle))
    {
        ::close(fd);
        ::close(profileDirectory);
        return {};
    }
    QByteArray ciphertext = file.read(16385);
    struct stat after{}, current{};
    const bool stable =
        file.error() == QFileDevice::NoError && ciphertext.size() == before.st_size && ::fstat(fd, &after) == 0 &&
        ::fstatat(profileDirectory, "identity.vault", &current, AT_SYMLINK_NOFOLLOW) == 0 &&
        before.st_dev == current.st_dev && before.st_ino == current.st_ino && before.st_size == after.st_size &&
        before.st_mtim.tv_sec == after.st_mtim.tv_sec && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
        before.st_ctim.tv_sec == after.st_ctim.tv_sec && before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
    ::close(profileDirectory);
    const QByteArray digest = stable ? QCryptographicHash::hash(ciphertext, QCryptographicHash::Sha256) : QByteArray();
    ciphertext.fill(0);
    return digest;
}

EnrollmentSession::ProfileFreshness queryProfileFreshness(uid_t uid)
{
    const QByteArray digest = localProfileDigest();
    if (digest.size() != 32)
        return EnrollmentSession::ProfileFreshness::Unknown;
    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return EnrollmentSession::ProfileFreshness::Unknown;
    timeval timeout{};
    timeout.tv_usec = 250000;
    (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strcpy(address.sun_path, "/run/kfaceauth/kfaceauthd.sock");
    auto freshness = SystemAuthProtocol::ProfileFreshness::Unknown;
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0)
    {
        const QByteArray request = SystemAuthProtocol::freshnessRequest(static_cast<quint32>(uid), digest);
        uint32_t responseLengthBe = 0;
        if (writeAll(fd, reinterpret_cast<const uint8_t *>(request.constData()), static_cast<size_t>(request.size())) &&
            readAll(fd, reinterpret_cast<uint8_t *>(&responseLengthBe), sizeof(responseLengthBe)) &&
            be32toh(responseLengthBe) == 5)
        {
            QByteArray response(5, Qt::Uninitialized);
            if (readAll(fd, reinterpret_cast<uint8_t *>(response.data()), 5))
                freshness = SystemAuthProtocol::parseFreshnessResponse(response);
        }
    }
    ::close(fd);
    if (localProfileDigest() != digest)
        return EnrollmentSession::ProfileFreshness::Unknown;
    switch (freshness)
    {
    case SystemAuthProtocol::ProfileFreshness::Current:
        return EnrollmentSession::ProfileFreshness::Current;
    case SystemAuthProtocol::ProfileFreshness::Stale:
        return EnrollmentSession::ProfileFreshness::Stale;
    case SystemAuthProtocol::ProfileFreshness::Unknown:
        return EnrollmentSession::ProfileFreshness::Unknown;
    }
    return EnrollmentSession::ProfileFreshness::Unknown;
}

bool systemProfileArtifactsExist(uid_t uid)
{
    const QString uidText = QString::number(uid);
    return QFileInfo::exists(QStringLiteral("/var/lib/kfaceauth/%1/identity.vault").arg(uidText)) ||
           QFileInfo::exists(QStringLiteral("/etc/kfaceauth/keys/%1.key").arg(uidText));
}
} // namespace

EnrollmentSession::EnrollmentSession(CameraPreviewSession *preview, IdentityWorkerClient *worker,
                                     KWalletKeyProvider *keyProvider, QObject *parent)
    : EnrollmentSession(preview, worker, keyProvider, {}, {}, parent)
{
}

EnrollmentSession::EnrollmentSession(CameraPreviewSession *preview, IdentityWorkerClient *worker,
                                     KWalletKeyProvider *keyProvider, AuthStatusProbe statusProbe,
                                     AuthOperationRunner operationRunner, QObject *parent)
    : QObject(parent), m_preview(preview), m_worker(worker), m_keyProvider(keyProvider),
      m_statusText(translate("Check your face profile or start a new enrollment.")),
      m_authStatusProbe(std::move(statusProbe)), m_authOperationRunner(std::move(operationRunner))
{
    Q_ASSERT(m_preview);
    Q_ASSERT(m_worker);
    Q_ASSERT(m_keyProvider);
    m_authReadbackTimer.setSingleShot(true);
    m_authReadbackTimer.setInterval(2000);
    connect(&m_authReadbackTimer, &QTimer::timeout, this,
            [this]()
            {
                if (m_authOperationState != AuthOperationState::Checking)
                    return;
                ++m_authPolicyQueryGeneration;
                m_authPolicyQueriesRemaining = 0;
                m_authPolicyStatusReady = false;
                m_sddmAuthMode = QStringLiteral("unknown");
                m_plasmaLockAuthMode = QStringLiteral("unknown");
                refreshAuthTargetStatus(QStringLiteral("sddm"));
                refreshAuthTargetStatus(QStringLiteral("plasma-lock"));
                m_authOperationState = AuthOperationState::Idle;
                m_authOperationResult = AuthOperationResult::ReadbackFailed;
                m_systemAuthBusy = false;
                Q_EMIT systemAuthChanged();
            });
    m_sessionTimer.setInterval(1000);
    connect(&m_sessionTimer, &QTimer::timeout, this,
            [this]()
            {
                m_remainingSeconds = m_preview->remainingSeconds();
                if (m_remainingSeconds <= 0)
                {
                    cancel();
                    m_statusText = translate("Enrollment timed out. No profile was saved.");
                }
                Q_EMIT stateChanged();
            });
    connect(m_preview, &CameraPreviewSession::stateChanged, this,
            [this]()
            {
                if (m_preview->state() != CameraPreviewSession::State::Streaming &&
                    (m_state == State::Enrolling || m_state == State::Capturing || m_state == State::ReadyToSave ||
                     m_state == State::Saving))
                    cancel();
                else
                    Q_EMIT stateChanged();
            });
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState state)
            {
                if (state != Qt::ApplicationActive)
                {
                    if (m_sessionTimer.isActive())
                        m_sessionTimer.stop();
                }
                else
                {
                    if (enrollmentActive() && m_remainingSeconds > 0 && !m_sessionTimer.isActive())
                        m_sessionTimer.start();
                }
            });
    connect(this, &EnrollmentSession::profileChanged, this,
            [this]()
            {
                refreshAuthTargetStatus(QStringLiteral("sddm"));
                refreshAuthTargetStatus(QStringLiteral("plasma-lock"));
                Q_EMIT systemAuthChanged();
            });
    checkSystemAuthStatus();
}

EnrollmentSession::~EnrollmentSession()
{
    cancel();
}

EnrollmentSession::State EnrollmentSession::state() const
{
    return m_state;
}

EnrollmentSession::ProfileState EnrollmentSession::profileState() const
{
    return m_profileState;
}

int EnrollmentSession::sampleCount() const
{
    return m_sampleCount;
}

int EnrollmentSession::storedSampleCount() const
{
    return m_storedSampleCount;
}

int EnrollmentSession::minimumSamples() const
{
    return 3;
}

int EnrollmentSession::recommendedSamples() const
{
    return 5;
}

int EnrollmentSession::maximumSamples() const
{
    return 8;
}

int EnrollmentSession::remainingSeconds() const
{
    return enrollmentActive() ? m_preview->remainingSeconds() : 0;
}

bool EnrollmentSession::busy() const
{
    return m_state == State::OpeningWallet || m_state == State::Capturing || m_state == State::Saving;
}

bool EnrollmentSession::enrollmentActive() const
{
    return m_state == State::OpeningWallet || m_state == State::Enrolling || m_state == State::Capturing ||
           m_state == State::ReadyToSave || m_state == State::Saving;
}

bool EnrollmentSession::canStartEnrollment() const
{
    return m_pageActive && !m_worker->busy() && !enrollmentActive() &&
           m_preview->state() == CameraPreviewSession::State::Streaming && m_preview->frameAvailable();
}

bool EnrollmentSession::canCancel() const
{
    return enrollmentActive();
}

bool EnrollmentSession::canCapture() const
{
    return m_pageActive && !m_worker->busy() && (m_state == State::Enrolling || m_state == State::ReadyToSave) &&
           m_sampleCount < maximumSamples() && m_preview->state() == CameraPreviewSession::State::Streaming &&
           m_preview->frameAvailable();
}

bool EnrollmentSession::canFinish() const
{
    return m_pageActive && !m_worker->busy() && m_state == State::ReadyToSave && m_sampleCount >= minimumSamples();
}

bool EnrollmentSession::enrollmentComplete() const
{
    return m_state == State::Complete;
}

bool EnrollmentSession::profileReady() const
{
    return m_profileState == ProfileState::Ready;
}

bool EnrollmentSession::replacementConfirmationRequired() const
{
    // An unavailable status cannot establish that overwriting is harmless.
    return m_profileState != ProfileState::Absent;
}

bool EnrollmentSession::profileNeedsAttention() const
{
    return m_profileState == ProfileState::Unreadable || m_profileState == ProfileState::ModelMismatch ||
           m_profileState == ProfileState::VaultLocked || m_profileState == ProfileState::Unavailable;
}

QString EnrollmentSession::profileStatusText() const
{
    switch (m_profileState)
    {
    case ProfileState::Absent:
        return translate("No face profile is enrolled.");
    case ProfileState::Ready:
        return m_storedSampleCount == 1 ? translate("%1 encrypted sample is enrolled.").arg(m_storedSampleCount)
                                        : translate("%1 encrypted samples are enrolled.").arg(m_storedSampleCount);
    case ProfileState::Unreadable:
        return translate("The encrypted profile is unreadable or corrupt.");
    case ProfileState::ModelMismatch:
        return translate("The profile model version does not match this runtime.");
    case ProfileState::VaultLocked:
        return translate("KWallet is locked or access was cancelled.");
    case ProfileState::Unavailable:
        return translate("Profile status is unavailable.");
    case ProfileState::Unknown:
    case ProfileState::Checking:
        return translate("Checking profile status…");
    }
    return translate("Profile status is unavailable.");
}

QString EnrollmentSession::statusText() const
{
    return m_statusText;
}

QString EnrollmentSession::errorCode() const
{
    return m_errorCode;
}

EnrollmentSession::AuthTargetStatus EnrollmentSession::sddmAuthStatus() const
{
    return m_sddmAuthStatus;
}

bool EnrollmentSession::sddmAuthConfigured() const
{
    return m_sddmAuthMode == QLatin1String("manual") || m_sddmAuthMode == QLatin1String("on-activity");
}

bool EnrollmentSession::sddmAuthEnabled() const
{
    return sddmAuthConfigured() && m_sddmAuthStatus == AuthTargetStatus::Enabled;
}

bool EnrollmentSession::sddmAuthCanEnable() const
{
    return profileReady() && m_authComponentsAvailable && m_sddmIntegrationAvailable;
}

QString EnrollmentSession::sddmAuthMode() const
{
    return m_sddmAuthMode;
}

QString EnrollmentSession::sddmIntegrationStatusText() const
{
    if (m_sddmIntegrationStatus == QLatin1String("available"))
        return translate("The patched SDDM face-authentication API is installed.");
    if (m_sddmIntegrationStatus == QLatin1String("theme-support-missing"))
        return translate("The active SDDM theme does not declare face-authentication support.");
    if (m_sddmIntegrationStatus == QLatin1String("sddm-api-missing"))
        return translate("The versioned SDDM face-authentication API is not installed.");
    if (m_sddmIntegrationStatus == QLatin1String("pam-template-missing"))
        return translate("The dedicated SDDM PAM service template is missing.");
    if (m_sddmIntegrationStatus == QLatin1String("pam-service-missing"))
        return translate("The dedicated SDDM face-authentication service is not prepared.");
    if (m_sddmIntegrationStatus == QLatin1String("auth-components-missing"))
        return translate("The experimental authentication package is not installed.");
    return translate("Patched SDDM integration availability is unknown.");
}

QString EnrollmentSession::sddmAuthStatusText() const
{
    switch (m_sddmAuthStatus)
    {
    case AuthTargetStatus::MissingComponents:
        return translate("Experimental authentication components are not installed.");
    case AuthTargetStatus::Off:
        return translate("Off. SDDM password sign-in remains unchanged.");
    case AuthTargetStatus::Ready:
        return m_sddmAuthErrorCode.isEmpty()
                   ? translate("Ready. Choose a face-authentication behavior; the password path remains available.")
                   : translate("Ready; the last change failed (%1).").arg(m_sddmAuthErrorCode);
    case AuthTargetStatus::Enabled:
        if (m_sddmAuthMode == QLatin1String("on-activity"))
            return translate("On activity. Password sign-in remains available at all times.");
        return translate("Button only. Password sign-in remains available at all times.");
    case AuthTargetStatus::Blocked:
        if (m_sddmAuthErrorCode == QLatin1String("sddm-theme-missing"))
            return translate("The active SDDM theme does not declare face-authentication support.");
        if (m_sddmAuthErrorCode == QLatin1String("sddm-api-missing"))
            return translate("Unavailable: install the versioned SDDM integration build.");
        if (m_sddmAuthErrorCode == QLatin1String("sddm-pam-service-missing"))
            return translate("Unavailable: prepare the dedicated SDDM face-authentication service.");
        if (m_sddmAuthErrorCode == QLatin1String("auth-policy-unknown"))
            return translate("The saved per-user setting could not be read safely.");
        if (m_sddmAuthErrorCode == QLatin1String("system-profile-unavailable"))
            return translate("The system profile is missing. Choose Sync system profile to approve a fresh copy.");
        if (m_sddmAuthErrorCode == QLatin1String("daemon-not-ready"))
            return translate("The experimental authentication service is not ready.");
        return translate("Unavailable until the required integration and profile are ready.");
    }
    return translate("Blocked: status-unavailable");
}

QString EnrollmentSession::sddmAuthErrorCode() const
{
    return m_sddmAuthErrorCode;
}

EnrollmentSession::AuthTargetStatus EnrollmentSession::plasmaLockAuthStatus() const
{
    return m_plasmaLockAuthStatus;
}

bool EnrollmentSession::plasmaLockAuthConfigured() const
{
    return m_plasmaLockAuthMode == QLatin1String("manual") || m_plasmaLockAuthMode == QLatin1String("on-activity");
}

bool EnrollmentSession::plasmaLockAuthEnabled() const
{
    return plasmaLockAuthConfigured() && m_plasmaLockAuthStatus == AuthTargetStatus::Enabled;
}

bool EnrollmentSession::plasmaLockAuthCanEnable() const
{
    return profileReady() && m_authComponentsAvailable && m_plasmaLockIntegrationAvailable;
}

QString EnrollmentSession::plasmaLockAuthMode() const
{
    return m_plasmaLockAuthMode;
}

QString EnrollmentSession::plasmaLockIntegrationStatusText() const
{
    if (m_plasmaLockIntegrationStatus == QLatin1String("available"))
        return translate("The experimental KScreenLocker face factor is installed. The unlock theme must register "
                         "interface v1 at runtime before a face attempt can start.");
    if (m_plasmaLockIntegrationStatus == QLatin1String("auth-components-missing"))
        return translate("The experimental authentication package is not installed.");
    if (m_plasmaLockIntegrationStatus == QLatin1String("pam-service-missing"))
        return translate("The dedicated Plasma unlock service is not prepared.");
    if (m_plasmaLockIntegrationStatus == QLatin1String("kscreenlocker-api-missing"))
        return translate("The versioned KScreenLocker face-authentication API is not installed.");
    if (m_plasmaLockIntegrationStatus == QLatin1String("pam-template-missing"))
        return translate("The dedicated Plasma PAM service template is missing.");
    return translate("The patched KScreenLocker integration is not available.");
}

QString EnrollmentSession::plasmaLockAuthStatusText() const
{
    switch (m_plasmaLockAuthStatus)
    {
    case AuthTargetStatus::MissingComponents:
        return translate("Experimental authentication components are not installed.");
    case AuthTargetStatus::Off:
        return translate("Off. The normal Plasma password unlock remains unchanged.");
    case AuthTargetStatus::Ready:
        return m_plasmaLockAuthErrorCode.isEmpty()
                   ? translate("Ready. Choose a face-authentication behavior; password unlock remains available.")
                   : translate("Ready; the last change failed (%1).").arg(m_plasmaLockAuthErrorCode);
    case AuthTargetStatus::Enabled:
        if (m_plasmaLockAuthMode == QLatin1String("on-activity"))
            return translate("On activity after the unlock view is shown. Password unlock stays available.");
        return translate("Button only. Password unlock stays available.");
    case AuthTargetStatus::Blocked:
        if (m_plasmaLockAuthErrorCode == QLatin1String("kscreenlocker-api-missing"))
            return translate("Unavailable: install the versioned KScreenLocker integration build.");
        if (m_plasmaLockAuthErrorCode == QLatin1String("kscreenlocker-theme-missing"))
            return translate("The active lock-screen theme has not confirmed the face-authentication interface.");
        if (m_plasmaLockAuthErrorCode == QLatin1String("kde-pam-service-missing"))
            return translate("Unavailable: prepare the dedicated Plasma unlock service.");
        if (m_plasmaLockAuthErrorCode == QLatin1String("auth-policy-unknown"))
            return translate("The saved per-user setting could not be read safely.");
        if (m_plasmaLockAuthErrorCode == QLatin1String("system-profile-unavailable"))
            return translate("The system profile is missing. Choose Sync system profile to approve a fresh copy.");
        if (m_plasmaLockAuthErrorCode == QLatin1String("daemon-not-ready"))
            return translate("The experimental authentication service is not ready.");
        return translate("Unavailable until the required integration and profile are ready.");
    }
    return translate("Blocked: status-unavailable");
}

QString EnrollmentSession::plasmaLockAuthErrorCode() const
{
    return m_plasmaLockAuthErrorCode;
}

EnrollmentSession::ProfileFreshness EnrollmentSession::systemProfileFreshness() const
{
    return m_systemProfileFreshness;
}

QString EnrollmentSession::systemProfileFreshnessCode() const
{
    switch (m_systemProfileFreshness)
    {
    case ProfileFreshness::Current:
        return QStringLiteral("current");
    case ProfileFreshness::Stale:
        return QStringLiteral("stale");
    case ProfileFreshness::Unknown:
        return QStringLiteral("unknown");
    }
    return QStringLiteral("unknown");
}

QString EnrollmentSession::systemProfileFreshnessText() const
{
    if (m_systemProfileFreshness == ProfileFreshness::Current)
        return translate("Current: the installed system copy matches the approved local profile generation.");
    if (m_systemProfileFreshness == ProfileFreshness::Stale)
        return translate("Stale: the local profile has changed. Approve a fresh system-profile sync.");
    return translate("Unknown: profile synchronization could not be verified.");
}

bool EnrollmentSession::systemAuthBusy() const
{
    return m_systemAuthBusy;
}

void EnrollmentSession::updateAuthTargetStatus(const QString &target, const QString &mode, bool authComponentsInstalled,
                                               bool integrationAvailable, bool pamServiceAvailable,
                                               bool systemProfileReady, bool daemonReady)
{
    AuthTargetStatus status = AuthTargetStatus::Off;
    QString errorCode;
    if (!authComponentsInstalled)
    {
        status = AuthTargetStatus::MissingComponents;
        errorCode = QStringLiteral("auth-components-not-installed");
    }
    else if (mode == QLatin1String("unknown"))
    {
        status = AuthTargetStatus::Blocked;
        errorCode = QStringLiteral("auth-policy-unknown");
    }
    else if (mode != QLatin1String("off"))
    {
        if (!integrationAvailable)
        {
            status = AuthTargetStatus::Blocked;
            if (target == QLatin1String("sddm"))
                errorCode = m_sddmIntegrationStatus == QLatin1String("theme-support-missing")
                                ? QStringLiteral("sddm-theme-missing")
                                : QStringLiteral("sddm-api-missing");
            else
                errorCode = QStringLiteral("kscreenlocker-api-missing");
        }
        else if (!pamServiceAvailable)
        {
            status = AuthTargetStatus::Blocked;
            errorCode = target == QLatin1String("sddm") ? QStringLiteral("sddm-pam-service-missing")
                                                        : QStringLiteral("kde-pam-service-missing");
        }
        else if (!daemonReady)
        {
            status = AuthTargetStatus::Blocked;
            errorCode = QStringLiteral("daemon-not-ready");
        }
        else if (!systemProfileReady)
        {
            status = AuthTargetStatus::Blocked;
            errorCode = QStringLiteral("system-profile-unavailable");
        }
        else
        {
            status = AuthTargetStatus::Enabled;
        }
    }
    else if (profileReady() && integrationAvailable)
    {
        status = AuthTargetStatus::Ready;
    }

    if (target == QLatin1String("sddm"))
    {
        m_sddmAuthStatus = status;
        m_sddmAuthMode = mode;
        m_sddmAuthConfigured = mode == QLatin1String("manual") || mode == QLatin1String("on-activity");
        m_sddmAuthErrorCode = errorCode;
    }
    else
    {
        m_plasmaLockAuthStatus = status;
        m_plasmaLockAuthMode = mode;
        m_plasmaLockAuthConfigured = mode == QLatin1String("manual") || mode == QLatin1String("on-activity");
        m_plasmaLockAuthErrorCode = errorCode;
    }
}

void EnrollmentSession::checkSystemAuthStatus()
{
    if (m_systemAuthBusy && m_authOperationState != AuthOperationState::Checking)
        return;
    if (m_authStatusProbe)
    {
        const quint64 generation = ++m_authPolicyQueryGeneration;
        m_authPolicyQueriesRemaining = 1;
        m_authPolicyStatusReady = false;
        QPointer<EnrollmentSession> session(this);
        m_authStatusProbe(
            [session, generation](AuthStatusSnapshot snapshot)
            {
                if (!session || generation != session->m_authPolicyQueryGeneration)
                    return;
                session->applyAuthStatusSnapshot(snapshot);
                session->finishAuthReadback();
                Q_EMIT session->systemAuthChanged();
            });
        return;
    }
    const uid_t uid = getuid();
    const QString helperPath = QStringLiteral("/usr/libexec/kfaceauth-sync-vault");
    const QString policyPath = QStringLiteral("/usr/libexec/kfaceauth-policy");
    const QString pamModule64 = QStringLiteral("/usr/lib64/security/pam_kfaceauth.so");
    const QString pamModule = QStringLiteral("/usr/lib/security/pam_kfaceauth.so");
    const bool componentsInstalled =
        KFaceAuth::authComponentsAvailable(QFileInfo::exists(helperPath) && QFileInfo::exists(policyPath),
                                           QFileInfo::exists(pamModule64), QFileInfo::exists(pamModule));
    const QString sddmMarker = QStringLiteral("/usr/share/sddm/kfaceauth/sddm-face-auth-api-v1.conf");
    const QString plasmaMarker = QStringLiteral("/usr/share/kfaceauth/integrations/kscreenlocker.json");
    m_authReadiness.components = componentsInstalled ? Readiness::Available : Readiness::Missing;
    m_authReadiness.sddmApi = markerReadiness(sddmMarker, markerContainsLines);
    const bool sddmApiAvailable = m_authReadiness.sddmApi == Readiness::Available;
    const KFaceAuth::SddmThemeConfigPaths sddmThemePaths{{QStringLiteral("/usr/lib/sddm/sddm.conf.d")},
                                                         {QStringLiteral("/etc/sddm.conf.d")},
                                                         QStringLiteral("/etc/sddm.conf"),
                                                         QStringLiteral("/usr/share/sddm/themes")};
    const auto themeSupport = KFaceAuth::activeSddmThemeSupportsFaceAuthentication(sddmThemePaths);
    m_authReadiness.sddmTheme = themeSupport == KFaceAuth::SddmThemeSupport::Declared      ? Readiness::Available
                                : themeSupport == KFaceAuth::SddmThemeSupport::Unsupported ? Readiness::Incompatible
                                                                                           : Readiness::Unknown;
    const bool sddmThemeAvailable = m_authReadiness.sddmTheme == Readiness::Available;
    const bool sddmIntegrationAvailable = sddmApiAvailable && sddmThemeAvailable;
    m_authReadiness.plasmaApi = markerReadiness(plasmaMarker, kscreenLockerMarkerSupportsFaceAuthentication);
    const bool plasmaIntegrationAvailable = m_authReadiness.plasmaApi == Readiness::Available;
    m_authComponentsAvailable = componentsInstalled;
    m_sddmIntegrationAvailable = sddmIntegrationAvailable;
    m_plasmaLockIntegrationAvailable = plasmaIntegrationAvailable;
    const QString sddmPamService = QStringLiteral("/etc/pam.d/sddm-kfaceauth");
    const QString plasmaPamService = QStringLiteral("/etc/pam.d/kde-kfaceauth");
    const QString sddmPamTemplate = QStringLiteral("/usr/share/kfaceauth/pam/sddm-kfaceauth");
    const QString plasmaPamTemplate = QStringLiteral("/usr/share/kfaceauth/pam/kde-kfaceauth");

    bool daemonReady = false;
    bool systemProfileReady = false;
    m_authReadiness.daemon = Readiness::Unknown;
    m_authReadiness.systemProfile = Readiness::Unknown;
    m_authReadiness.sddmPam = QFileInfo::exists(sddmPamService) ? Readiness::Available : Readiness::Missing;
    m_authReadiness.plasmaPam = QFileInfo::exists(plasmaPamService) ? Readiness::Available : Readiness::Missing;
    const QString socketPath = QStringLiteral("/run/kfaceauth/kfaceauthd.sock");
    if (QFileInfo::exists(socketPath))
    {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd >= 0)
        {
            timeval timeout{};
            timeout.tv_usec = 250000;
            (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            (void)::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

            sockaddr_un address{};
            address.sun_family = AF_UNIX;
            const QByteArray socketBytes = socketPath.toLocal8Bit();
            std::memcpy(address.sun_path, socketBytes.constData(),
                        std::min(static_cast<size_t>(socketBytes.size()), sizeof(address.sun_path) - 1));

            if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0)
            {
                const QByteArray request = SystemAuthProtocol::statusRequest(static_cast<quint32>(uid));
                const bool sent = writeAll(fd, reinterpret_cast<const uint8_t *>(request.constData()),
                                           static_cast<size_t>(request.size()));
                uint32_t responseLengthBe = 0;
                const bool gotHeader =
                    sent && readAll(fd, reinterpret_cast<uint8_t *>(&responseLengthBe), sizeof(responseLengthBe));
                const uint32_t responseLength = be32toh(responseLengthBe);
                if (gotHeader && (responseLength == 4 || responseLength == 6))
                {
                    QByteArray response(static_cast<qsizetype>(responseLength), Qt::Uninitialized);
                    if (readAll(fd, reinterpret_cast<uint8_t *>(response.data()), static_cast<size_t>(response.size())))
                    {
                        const SystemAuthProtocol::Status status =
                            SystemAuthProtocol::parseStatusResponse(QByteArrayView(response));
                        daemonReady = status.daemonReady;
                        systemProfileReady = status.systemProfileReady;
                        m_authReadiness.daemon = status.daemonReady ? Readiness::Available : Readiness::Unknown;
                        m_authReadiness.systemProfile =
                            status.systemProfileKnown
                                ? (status.systemProfileReady ? Readiness::Available : Readiness::Missing)
                                : Readiness::Unknown;
                    }
                }
            }
            ::close(fd);
        }
    }

    m_sddmIntegrationStatus = !componentsInstalled ? QStringLiteral("auth-components-missing")
                              : !sddmApiAvailable  ? QStringLiteral("sddm-api-missing")
                              : m_authReadiness.sddmTheme == Readiness::Unknown
                                  ? QStringLiteral("theme-support-unknown")
                              : !sddmThemeAvailable                 ? QStringLiteral("theme-support-missing")
                              : !QFileInfo::exists(sddmPamTemplate) ? QStringLiteral("pam-template-missing")
                              : !QFileInfo::exists(sddmPamService)  ? QStringLiteral("pam-service-missing")
                                                                    : QStringLiteral("available");
    m_plasmaLockIntegrationStatus = !componentsInstalled          ? QStringLiteral("auth-components-missing")
                                    : !plasmaIntegrationAvailable ? QStringLiteral("kscreenlocker-api-missing")
                                    : !QFileInfo::exists(plasmaPamTemplate) ? QStringLiteral("pam-template-missing")
                                    : !QFileInfo::exists(plasmaPamService)  ? QStringLiteral("pam-service-missing")
                                                                            : QStringLiteral("available");
    m_daemonReady = daemonReady;
    m_systemProfileReady = systemProfileReady;
    m_systemProfileFreshness = systemProfileReady ? queryProfileFreshness(uid) : ProfileFreshness::Unknown;

    const QString policyReader = QStringLiteral("/usr/libexec/kfaceauth-policy");
    const QString policyStore = QStringLiteral("/etc/kfaceauth/policy.json");
    const bool policyReaderAvailable = QFileInfo::exists(policyReader);
    const bool policyStoreAvailable = QFileInfo::exists(policyStore);
    const quint64 queryGeneration = ++m_authPolicyQueryGeneration;
    m_authPolicyQueriesRemaining = 0;
    m_authPolicyStatusReady = !policyReaderAvailable && !policyStoreAvailable;
    m_sddmAuthMode = policyReaderAvailable || policyStoreAvailable ? QStringLiteral("unknown") : QStringLiteral("off");
    m_plasmaLockAuthMode = m_sddmAuthMode;
    refreshAuthTargetStatus(QStringLiteral("sddm"));
    refreshAuthTargetStatus(QStringLiteral("plasma-lock"));

    if (policyReaderAvailable)
    {
        m_authPolicyStatusReady = false;
        m_authPolicyQueriesRemaining = 2;
        const auto startPolicyQuery = [this, uid, queryGeneration, &policyReader](const QString &target)
        {
            auto *process = new QProcess(this);
            auto *deadline = new QTimer(process);
            deadline->setSingleShot(true);
            auto completed = std::make_shared<bool>(false);
            auto finish = [this, process, deadline, completed, queryGeneration, target](const QString &mode)
            {
                if (std::exchange(*completed, true))
                    return;
                deadline->stop();
                if (queryGeneration == m_authPolicyQueryGeneration)
                {
                    if (target == QLatin1String("sddm"))
                        m_sddmAuthMode = mode;
                    else
                        m_plasmaLockAuthMode = mode;
                    if (m_authPolicyQueriesRemaining > 0)
                        --m_authPolicyQueriesRemaining;
                    if (m_authPolicyQueriesRemaining == 0)
                        m_authPolicyStatusReady = m_sddmAuthMode != QLatin1String("unknown") &&
                                                  m_plasmaLockAuthMode != QLatin1String("unknown");
                    refreshAuthTargetStatus(target);
                    finishAuthReadback();
                    Q_EMIT systemAuthChanged();
                }
                process->deleteLater();
            };
            connect(deadline, &QTimer::timeout, process,
                    [process, finish]()
                    {
                        process->kill();
                        finish(QStringLiteral("unknown"));
                    });
            connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
                    [process, finish](int exitCode, QProcess::ExitStatus exitStatus)
                    {
                        const QString mode = parseAuthPolicyMode(process->readAllStandardOutput(),
                                                                 exitStatus == QProcess::NormalExit && exitCode == 0);
                        finish(mode);
                    });
            connect(process, &QProcess::errorOccurred, this,
                    [finish](QProcess::ProcessError) { finish(QStringLiteral("unknown")); });
            process->start(policyReader, {QStringLiteral("--get"), QStringLiteral("--uid"), QString::number(uid),
                                          QStringLiteral("--target"), target});
            deadline->start(500);
        };
        startPolicyQuery(QStringLiteral("sddm"));
        startPolicyQuery(QStringLiteral("plasma-lock"));
    }
    finishAuthReadback();
    Q_EMIT systemAuthChanged();
}

void EnrollmentSession::refreshAuthTargetStatus(const QString &target)
{
    const bool isSddm = target == QLatin1String("sddm");
    updateAuthTargetStatus(target, isSddm ? m_sddmAuthMode : m_plasmaLockAuthMode, m_authComponentsAvailable,
                           isSddm ? m_sddmIntegrationAvailable : m_plasmaLockIntegrationAvailable,
                           isSddm ? m_authReadiness.sddmPam == Readiness::Available
                                  : m_authReadiness.plasmaPam == Readiness::Available,
                           m_systemProfileReady, m_daemonReady);
}

void EnrollmentSession::enableSddmAuth()
{
    setSddmAuthMode(QStringLiteral("on-activity"));
}

void EnrollmentSession::disableSddmAuth()
{
    setSddmAuthMode(QStringLiteral("off"));
}

void EnrollmentSession::enablePlasmaLockAuth()
{
    setPlasmaLockAuthMode(QStringLiteral("on-activity"));
}

void EnrollmentSession::disablePlasmaLockAuth()
{
    setPlasmaLockAuthMode(QStringLiteral("off"));
}

void EnrollmentSession::setSddmAuthMode(const QString &mode)
{
    runAuthTargetOperation(QStringLiteral("sddm"), mode);
}

void EnrollmentSession::setPlasmaLockAuthMode(const QString &mode)
{
    runAuthTargetOperation(QStringLiteral("plasma-lock"), mode);
}

void EnrollmentSession::syncSystemProfile()
{
    runAuthTargetOperation(QStringLiteral("profile"), {}, true);
}

void EnrollmentSession::runAuthTargetOperation(const QString &target, const QString &mode, bool resync)
{
    if (m_systemAuthBusy || m_requestActive || busy() || enrollmentActive())
        return;
    const bool enabled = mode == QLatin1String("manual") || mode == QLatin1String("on-activity");
    if (!resync && (target != QLatin1String("sddm") && target != QLatin1String("plasma-lock")))
        return;
    if (!resync && mode != QLatin1String("off") && !enabled)
        return;
    if (!resync && mode == (target == QLatin1String("sddm") ? m_sddmAuthMode : m_plasmaLockAuthMode))
        return;

    m_authOperationTarget = target;
    if (!m_authPolicyStatusReady || (resync && !systemProfileCanSync()) ||
        (enabled && !(target == QLatin1String("sddm") ? sddmAuthCanEnable() : plasmaLockAuthCanEnable())))
    {
        m_authOperationResult = AuthOperationResult::NotReady;
        Q_EMIT systemAuthChanged();
        return;
    }
    m_expectedSddmMode = m_sddmAuthMode;
    m_expectedPlasmaMode = m_plasmaLockAuthMode;
    if (!resync)
        (target == QLatin1String("sddm") ? m_expectedSddmMode : m_expectedPlasmaMode) = mode;
    m_authOperationRequiresProfile = resync || enabled;
    m_authOperationRequiresFreshness = resync || enabled;
    m_systemAuthBusy = true;
    m_authOperationResult = AuthOperationResult::None;
    m_authOperationState =
        m_authOperationRequiresProfile ? AuthOperationState::OpeningWallet : AuthOperationState::Updating;
    const quint64 generation = ++m_authOperationGeneration;
    Q_EMIT systemAuthChanged();

    QPointer<EnrollmentSession> session(this);
    auto launch = [session, target, mode, resync, enabled, generation](QByteArray secret = {}) mutable
    {
        if (!session || generation != session->m_authOperationGeneration)
        {
            secret.fill('\0');
            return;
        }
        QStringList arguments{QStringLiteral("/usr/libexec/kfaceauth-sync-vault")};
        QByteArray input;
        if (resync || enabled)
        {
            if (resync)
                arguments << QStringLiteral("--resync-profile");
            else
                arguments << QStringLiteral("--enable-target") << target << QStringLiteral("--mode") << mode;
            const QByteArray dataHome = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation).toUtf8();
            input.append("KFAUTH01", 8);
            input.append(static_cast<char>((dataHome.size() >> 8) & 0xff));
            input.append(static_cast<char>(dataHome.size() & 0xff));
            input.append(dataHome);
            input.append(secret);
        }
        else
            arguments << QStringLiteral("--disable-target") << target;
        secret.fill('\0');
        secret.clear();
        session->m_authOperationState = AuthOperationState::Updating;
        Q_EMIT session->systemAuthChanged();
        auto completion = [session, generation](int exitCode, bool normalExit)
        {
            if (session && generation == session->m_authOperationGeneration &&
                session->m_authOperationState == AuthOperationState::Updating)
                session->finishAuthTargetOperation(exitCode, normalExit);
        };
        if (session->m_authOperationRunner)
        {
            session->m_authOperationRunner(std::move(arguments), std::move(input), std::move(completion));
            return;
        }
        auto *process = new QProcess(session);
        auto sensitiveInput = std::shared_ptr<QByteArray>(new QByteArray(std::move(input)),
                                                          [](QByteArray *bytes)
                                                          {
                                                              bytes->fill('\0');
                                                              delete bytes;
                                                          });
        connect(process, &QProcess::started, process,
                [process, sensitiveInput]()
                {
                    process->write(*sensitiveInput);
                    sensitiveInput->fill('\0');
                    sensitiveInput->clear();
                    process->closeWriteChannel();
                });
        auto finished = std::make_shared<bool>(false);
        auto completeOnce = [process, sensitiveInput, finished, completion](int code, bool normal)
        {
            if (std::exchange(*finished, true))
                return;
            sensitiveInput->fill('\0');
            sensitiveInput->clear();
            process->deleteLater();
            completion(code, normal);
        };
        connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), process,
                [completeOnce](int code, QProcess::ExitStatus status)
                { completeOnce(code, status == QProcess::NormalExit); });
        connect(process, &QProcess::errorOccurred, process,
                [completeOnce](QProcess::ProcessError) { completeOnce(-1, false); });
        process->start(QStringLiteral("pkexec"), arguments);
    };
    if (!m_authOperationRequiresProfile)
    {
        launch();
        return;
    }
    m_keyProvider->requestKey(
        [session, generation, launch = std::move(launch)](KWalletKeyProvider::Result result) mutable
        {
            if (!session || generation != session->m_authOperationGeneration)
            {
                result.clear();
                return;
            }
            if (result.state != KWalletKeyProvider::State::Available || result.key.size() != 32)
            {
                result.clear();
                session->m_systemAuthBusy = false;
                session->m_authOperationState = AuthOperationState::Idle;
                session->m_authOperationResult = AuthOperationResult::WalletUnavailable;
                session->checkSystemAuthStatus();
                Q_EMIT session->systemAuthChanged();
                return;
            }
            QByteArray key = std::move(result.key);
            result.clear();
            launch(std::move(key));
        });
}

void EnrollmentSession::finishAuthTargetOperation(int exitCode, bool normalExit)
{
    if (normalExit && exitCode == 0)
    {
        m_authOperationState = AuthOperationState::Checking;
        m_authReadbackTimer.start();
        checkSystemAuthStatus();
        return;
    }
    m_systemAuthBusy = false;
    m_authOperationState = AuthOperationState::Idle;
    m_authOperationResult = normalExit && (exitCode == 126 || exitCode == 127) ? AuthOperationResult::Cancelled
                                                                               : AuthOperationResult::Failed;
    checkSystemAuthStatus();
    Q_EMIT systemAuthChanged();
}

void EnrollmentSession::finishAuthReadback()
{
    if (m_authOperationState != AuthOperationState::Checking || m_authPolicyQueriesRemaining != 0)
        return;
    const bool verified =
        (!m_authOperationRequiresFreshness || m_systemProfileFreshness == ProfileFreshness::Current) &&
        m_authPolicyStatusReady && m_sddmAuthMode == m_expectedSddmMode &&
        m_plasmaLockAuthMode == m_expectedPlasmaMode &&
        (!m_authOperationRequiresProfile ||
         (m_authReadiness.daemon == Readiness::Available && m_authReadiness.systemProfile == Readiness::Available));
    m_authOperationResult = verified ? AuthOperationResult::Success : AuthOperationResult::ReadbackFailed;
    m_authReadbackTimer.stop();
    m_authOperationState = AuthOperationState::Idle;
    m_systemAuthBusy = false;
    Q_EMIT systemAuthChanged();
}

void EnrollmentSession::applyAuthStatusSnapshot(const AuthStatusSnapshot &snapshot)
{
    m_authReadiness = snapshot;
    m_authComponentsAvailable = snapshot.components == Readiness::Available;
    m_sddmIntegrationAvailable = snapshot.sddmApi == Readiness::Available && snapshot.sddmTheme == Readiness::Available;
    m_plasmaLockIntegrationAvailable = snapshot.plasmaApi == Readiness::Available;
    m_daemonReady = snapshot.daemon == Readiness::Available;
    m_systemProfileReady = snapshot.systemProfile == Readiness::Available;
    m_systemProfileFreshness = snapshot.freshness;
    if (m_systemProfileFreshness != ProfileFreshness::Current && m_systemProfileFreshness != ProfileFreshness::Stale)
        m_systemProfileFreshness = ProfileFreshness::Unknown;
    auto validMode = [](const QString &mode)
    {
        return mode == QLatin1String("off") || mode == QLatin1String("manual") || mode == QLatin1String("on-activity")
                   ? mode
                   : QStringLiteral("unknown");
    };
    m_sddmAuthMode = validMode(snapshot.sddmMode);
    m_plasmaLockAuthMode = validMode(snapshot.plasmaMode);
    m_authPolicyQueriesRemaining = 0;
    m_authPolicyStatusReady =
        m_sddmAuthMode != QLatin1String("unknown") && m_plasmaLockAuthMode != QLatin1String("unknown");
    m_sddmIntegrationStatus = !m_authComponentsAvailable                   ? QStringLiteral("auth-components-missing")
                              : snapshot.sddmApi != Readiness::Available   ? QStringLiteral("sddm-api-missing")
                              : snapshot.sddmTheme == Readiness::Unknown   ? QStringLiteral("theme-support-unknown")
                              : snapshot.sddmTheme != Readiness::Available ? QStringLiteral("theme-support-missing")
                              : snapshot.sddmPam != Readiness::Available   ? QStringLiteral("pam-service-missing")
                                                                           : QStringLiteral("available");
    m_plasmaLockIntegrationStatus = !m_authComponentsAvailable ? QStringLiteral("auth-components-missing")
                                    : snapshot.plasmaApi != Readiness::Available
                                        ? QStringLiteral("kscreenlocker-api-missing")
                                    : snapshot.plasmaPam != Readiness::Available ? QStringLiteral("pam-service-missing")
                                                                                 : QStringLiteral("available");
    updateAuthTargetStatus(QStringLiteral("sddm"), m_sddmAuthMode, m_authComponentsAvailable,
                           m_sddmIntegrationAvailable, snapshot.sddmPam == Readiness::Available, m_systemProfileReady,
                           m_daemonReady);
    updateAuthTargetStatus(QStringLiteral("plasma-lock"), m_plasmaLockAuthMode, m_authComponentsAvailable,
                           m_plasmaLockIntegrationAvailable, snapshot.plasmaPam == Readiness::Available,
                           m_systemProfileReady, m_daemonReady);
}

bool EnrollmentSession::systemProfileCanSync() const
{
    return profileReady() && m_authComponentsAvailable && m_authPolicyStatusReady && !m_systemAuthBusy &&
           !m_requestActive && !busy() && !enrollmentActive();
}

QVariantList EnrollmentSession::authReadinessRows(bool sddm) const
{
    QVariantList rows;
    auto row = [&rows](const QString &id, const QString &label, Readiness state)
    {
        rows.append(QVariantMap{{QStringLiteral("id"), id},
                                {QStringLiteral("label"), label},
                                {QStringLiteral("code"), readinessCode(state)},
                                {QStringLiteral("value"), readinessText(state)},
                                {QStringLiteral("ready"), state == Readiness::Available}});
    };
    row(QStringLiteral("components"), translate("Experimental components"), m_authReadiness.components);
    row(QStringLiteral("integration"),
        sddm ? translate("SDDM 0.21.0 / API v1") : translate("KScreenLocker 6.7.5 / API v1"),
        sddm ? m_authReadiness.sddmApi : m_authReadiness.plasmaApi);
    row(QStringLiteral("theme-declaration"), translate("Theme declaration"),
        sddm ? m_authReadiness.sddmTheme : Readiness::Unknown);
    row(QStringLiteral("theme-runtime"), translate("Runtime theme registration"), Readiness::Unknown);
    row(QStringLiteral("pam-service"), translate("Dedicated PAM service"),
        sddm ? m_authReadiness.sddmPam : m_authReadiness.plasmaPam);
    row(QStringLiteral("daemon"), translate("Authentication service"), m_authReadiness.daemon);
    const Readiness local = profileReady()                           ? Readiness::Available
                            : m_profileState == ProfileState::Absent ? Readiness::Missing
                                                                     : Readiness::Unknown;
    row(QStringLiteral("local-profile"), translate("Local profile"), local);
    row(QStringLiteral("system-profile"), translate("System profile"), m_authReadiness.systemProfile);
    const QString mode = sddm ? m_sddmAuthMode : m_plasmaLockAuthMode;
    rows.append(QVariantMap{{QStringLiteral("id"), QStringLiteral("policy")},
                            {QStringLiteral("label"), translate("Saved mode")},
                            {QStringLiteral("code"), mode},
                            {QStringLiteral("value"), mode == QLatin1String("off")           ? translate("Off")
                                                      : mode == QLatin1String("manual")      ? translate("Button only")
                                                      : mode == QLatin1String("on-activity") ? translate("On activity")
                                                                                             : translate("Unknown")},
                            {QStringLiteral("ready"), mode != QLatin1String("unknown")}});
    return rows;
}

QVariantList EnrollmentSession::sddmReadiness() const
{
    return authReadinessRows(true);
}
QVariantList EnrollmentSession::plasmaLockReadiness() const
{
    return authReadinessRows(false);
}
EnrollmentSession::AuthOperationState EnrollmentSession::authOperationState() const
{
    return m_authOperationState;
}
EnrollmentSession::AuthOperationResult EnrollmentSession::authOperationResult() const
{
    return m_authOperationResult;
}
QString EnrollmentSession::authOperationTarget() const
{
    return m_authOperationTarget;
}

QString EnrollmentSession::authOperationErrorCode() const
{
    switch (m_authOperationResult)
    {
    case AuthOperationResult::None:
    case AuthOperationResult::Success:
        return {};
    case AuthOperationResult::Cancelled:
        return QStringLiteral("auth-change-cancelled");
    case AuthOperationResult::WalletUnavailable:
        return QStringLiteral("auth-wallet-unavailable");
    case AuthOperationResult::NotReady:
        return QStringLiteral("auth-change-not-ready");
    case AuthOperationResult::Failed:
        return QStringLiteral("auth-change-failed");
    case AuthOperationResult::ReadbackFailed:
        return QStringLiteral("auth-readback-failed");
    }
    return QStringLiteral("auth-change-failed");
}

QString EnrollmentSession::authOperationText() const
{
    switch (m_authOperationState)
    {
    case AuthOperationState::OpeningWallet:
        return translate("Waiting for KWallet access…");
    case AuthOperationState::Updating:
        return translate("Updating experimental authentication…");
    case AuthOperationState::Checking:
        return translate("Checking the saved authentication settings…");
    case AuthOperationState::Idle:
        break;
    }
    switch (m_authOperationResult)
    {
    case AuthOperationResult::None:
        return {};
    case AuthOperationResult::Success:
        return m_authOperationTarget == QLatin1String("profile")
                   ? translate("System profile synchronized; saved modes were verified.")
                   : translate("Authentication change verified.");
    case AuthOperationResult::Cancelled:
        return translate("The change was cancelled or denied. Review the saved settings before retrying.");
    case AuthOperationResult::WalletUnavailable:
        return translate("KWallet access was unavailable. No authentication change was started.");
    case AuthOperationResult::NotReady:
        return translate("Check the profile and integration requirements, refresh status, then retry.");
    case AuthOperationResult::Failed:
        return translate("The authentication change failed. Refresh status before retrying.");
    case AuthOperationResult::ReadbackFailed:
        return translate("The saved settings could not be verified. Refresh status before retrying.");
    }
    return {};
}

EnrollmentSession::GuidePhase EnrollmentSession::guidePhase() const
{
    if (m_state == State::Saving)
        return GuidePhase::Saving;
    if (m_state == State::Complete)
        return GuidePhase::Complete;
    if (m_state == State::Failed)
        return GuidePhase::Error;
    if (!enrollmentActive())
        return GuidePhase::Camera;
    switch (m_sampleCount)
    {
    case 0:
        return GuidePhase::Frontal;
    case 1:
        return GuidePhase::Left;
    case 2:
        return GuidePhase::Right;
    case 3:
        return GuidePhase::Tilt;
    case 4:
        return GuidePhase::Natural;
    default:
        return GuidePhase::Review;
    }
}

void EnrollmentSession::refreshProfileStatus()
{
    if (m_requestActive || busy())
        return;
    m_profileState = ProfileState::Checking;
    Q_EMIT profileChanged();
    m_keyProvider->requestKey(
        [this](KWalletKeyProvider::Result result)
        {
            if (result.state == KWalletKeyProvider::State::Available)
            {
                runStatus(result.key);
            }
            else if (result.state == KWalletKeyProvider::State::Absent)
            {
                runStatus({});
            }
            else
            {
                m_profileState = result.state == KWalletKeyProvider::State::Locked ||
                                         result.state == KWalletKeyProvider::State::Cancelled
                                     ? ProfileState::VaultLocked
                                     : ProfileState::Unavailable;
                Q_EMIT profileChanged();
            }
            result.clear();
        });
}

void EnrollmentSession::runStatus(const QByteArray &key)
{
    const quint64 generation = nextGeneration();
    QByteArray request = IdentityProtocol::statusRequest(generation, key);
    if (request.isEmpty())
    {
        m_profileState = ProfileState::Unavailable;
        Q_EMIT profileChanged();
        checkSystemAuthStatus();
        return;
    }
    m_pendingOperation = PendingOperation::Status;
    m_requestActive = true;
    m_activeGeneration = generation;
    m_worker->execute(generation, std::move(request),
                      [this](quint64 completed, QByteArrayView payload, const QString &error)
                      { handleResponse(completed, payload, error); });
}

void EnrollmentSession::startEnrollment()
{
    if (!m_pageActive || busy() || m_worker->busy() || m_preview->state() != CameraPreviewSession::State::Streaming)
    {
        fail(QStringLiteral("preview-required"), translate("Start the camera preview before enrollment."));
        return;
    }
    clearSensitive();
    setState(State::OpeningWallet, translate("Opening your user-session wallet…"));
    m_keyProvider->requestKey(
        [this](KWalletKeyProvider::Result result)
        {
            if (result.state == KWalletKeyProvider::State::Available)
            {
                m_key = std::move(result.key);
                m_keyNeedsStore = false;
            }
            else if (result.state == KWalletKeyProvider::State::Absent)
            {
                result.clear();
                result = m_keyProvider->generateTransientKey();
                if (result.state == KWalletKeyProvider::State::Available)
                {
                    m_key = std::move(result.key);
                    m_keyNeedsStore = true;
                }
            }
            if (m_key.size() != IdentityProtocol::KeyBytes)
            {
                result.clear();
                fail(QStringLiteral("vault-key-unavailable"),
                     translate("The user-session vault key is locked, cancelled, or unavailable."));
                return;
            }
            result.clear();
            if (!m_pageActive || !m_preview->beginEnrollmentBudget())
            {
                fail(QStringLiteral("preview-consent-expired"),
                     translate("Restart the camera preview before starting a new registration."));
                return;
            }
            m_remainingSeconds = m_preview->remainingSeconds();
            m_sessionTimer.start();
            setState(State::Enrolling, translate("Capture three to five deliberate appearance samples."));
        });
}

void EnrollmentSession::captureSample(bool automatic)
{
    if (!canCapture())
        return;
    QImage frame;
    if (!m_preview->copyCurrentFrame(&frame))
    {
        fail(QStringLiteral("frame-unavailable"), translate("No current preview frame is available."));
        return;
    }
    const quint64 generation = nextGeneration();
    QString error;
    QByteArray request = IdentityProtocol::extractSampleRequest(generation, m_embeddings,
                                                                static_cast<quint8>(m_sampleCount), frame, &error);
    frame.fill(0);
    if (request.isEmpty())
    {
        fail(error.isEmpty() ? QStringLiteral("invalid-frame") : error,
             translate("The current frame could not be prepared safely."));
        return;
    }
    m_pendingOperation = PendingOperation::Capture;
    m_requestActive = true;
    m_captureAutomatic = automatic;
    m_activeGeneration = generation;
    setState(State::Capturing, translate("Extracting one local enrollment sample…"));
    m_worker->execute(generation, std::move(request),
                      [this](quint64 completed, QByteArrayView payload, const QString &transportError)
                      { handleResponse(completed, payload, transportError); });
}

void EnrollmentSession::discardLastSample()
{
    if (busy() || m_sampleCount <= 0)
        return;
    const qsizetype offset = (m_sampleCount - 1) * IdentityProtocol::EmbeddingBytes;
    std::fill_n(m_embeddings.data() + offset, IdentityProtocol::EmbeddingBytes, '\0');
    m_embeddings.truncate(offset);
    --m_sampleCount;
    setState(m_sampleCount >= minimumSamples() ? State::ReadyToSave : State::Enrolling,
             translate("The latest sample was discarded. Capture a replacement when ready."));
    Q_EMIT samplesChanged();
    Q_EMIT guidePhaseChanged();
}

void EnrollmentSession::finishAndSave()
{
    if (!canFinish())
        return;
    setState(State::Saving, translate("Encrypting and atomically saving your face profile…"));
    if (m_keyNeedsStore)
    {
        m_keyProvider->storeKey(m_key,
                                [this](KWalletKeyProvider::Result result)
                                {
                                    if (result.state != KWalletKeyProvider::State::Available)
                                    {
                                        result.clear();
                                        fail(QStringLiteral("vault-key-unavailable"),
                                             translate("The user-session vault key could not be stored safely."));
                                        return;
                                    }
                                    result.clear();
                                    m_keyNeedsStore = false;
                                    m_keyStoredDuringEnrollment = true;
                                    commitEnrollment();
                                });
        return;
    }
    commitEnrollment();
}

void EnrollmentSession::commitEnrollment()
{
    if (m_systemAuthBusy)
    {
        m_commitAfterSystemRevoke = false;
        fail(QStringLiteral("system-auth-operation-busy"),
             translate("Wait for the authentication settings operation to finish before saving the profile."));
        return;
    }
    if (!m_commitAfterSystemRevoke)
    {
        m_commitAfterSystemRevoke = true;
        const quint64 epoch = m_profileMutationEpoch;
        revokeSystemProfileThen(
            [this, epoch](bool success)
            {
                if (epoch != m_profileMutationEpoch || !m_commitAfterSystemRevoke)
                    return;
                if (!success)
                {
                    m_commitAfterSystemRevoke = false;
                    fail(QStringLiteral("system-profile-revoke-failed"),
                         translate(
                             "The previous system profile could not be revoked. The local profile was not changed."));
                    return;
                }
                m_commitAfterSystemRevoke = true;
                commitEnrollment();
            });
        return;
    }
    m_commitAfterSystemRevoke = false;

    const quint64 generation = nextGeneration();
    QByteArray request =
        IdentityProtocol::commitRequest(generation, m_key, m_embeddings, static_cast<quint8>(m_sampleCount));
    if (request.isEmpty())
    {
        fail(QStringLiteral("invalid-sample-state"), translate("The enrollment sample set is invalid."));
        return;
    }
    m_pendingOperation = PendingOperation::Commit;
    m_requestActive = true;
    m_activeGeneration = generation;
    m_worker->execute(generation, std::move(request),
                      [this](quint64 completed, QByteArrayView payload, const QString &error)
                      { handleResponse(completed, payload, error); });
}

void EnrollmentSession::revokeSystemProfileThen(std::function<void(bool)> continuation)
{
    const uid_t uid = getuid();
    const QString policyReader = QStringLiteral("/usr/libexec/kfaceauth-policy");
    const QString policyStore = QStringLiteral("/etc/kfaceauth/policy.json");
    const bool integrationPossible =
        m_authComponentsAvailable || QFileInfo::exists(policyReader) || QFileInfo::exists(policyStore);
    if (integrationPossible && m_authPolicyQueriesRemaining > 0)
    {
        // Wait for the existing bounded read instead of treating a pending read as a failure.
        const quint64 epoch = m_profileMutationEpoch;
        auto *timer = new QTimer(this);
        timer->setSingleShot(true);
        auto callback = std::make_shared<std::function<void(bool)>>(std::move(continuation));
        auto finish = [this, timer, callback, epoch](bool ready)
        {
            if (!*callback)
                return;
            auto continuation = std::move(*callback);
            *callback = {};
            timer->stop();
            disconnect(this, nullptr, timer, nullptr);
            timer->deleteLater();
            if (!ready || epoch != m_profileMutationEpoch)
                continuation(false);
            else
                revokeSystemProfileThen(std::move(continuation));
        };
        connect(this, &EnrollmentSession::systemAuthChanged, timer,
                [this, finish]
                {
                    if (m_authPolicyQueriesRemaining == 0)
                        finish(true);
                });
        connect(timer, &QTimer::timeout, this, [finish] { finish(false); });
        timer->start(2000);
        return;
    }
    if (integrationPossible && (!m_authPolicyStatusReady || m_sddmAuthMode == QLatin1String("unknown") ||
                                m_plasmaLockAuthMode == QLatin1String("unknown")))
    {
        continuation(false);
        return;
    }
    const QString &sddmMode = m_sddmAuthMode;
    const QString &plasmaMode = m_plasmaLockAuthMode;
    const bool policyEnabled = sddmMode == QLatin1String("manual") || sddmMode == QLatin1String("on-activity") ||
                               plasmaMode == QLatin1String("manual") || plasmaMode == QLatin1String("on-activity");
    const bool systemProfileExists = systemProfileArtifactsExist(uid) || m_systemProfileReady;
    if (!policyEnabled && !systemProfileExists)
    {
        continuation(true);
        return;
    }

    const QString helper = QStringLiteral("/usr/libexec/kfaceauth-sync-vault");
    if (!QFileInfo::exists(helper) || m_systemAuthBusy)
    {
        continuation(false);
        return;
    }

    m_systemAuthBusy = true;
    auto *process = new QProcess(this);
    m_systemProfileMutationProcess = process;
    const QString uidText = QString::number(uid);
    auto continuationState = std::make_shared<std::function<void(bool)>>(std::move(continuation));
    auto complete = [this, process, continuationState](bool success) mutable
    {
        if (m_systemProfileMutationProcess != process)
            return;
        m_systemProfileMutationProcess = nullptr;
        m_systemAuthBusy = false;
        process->deleteLater();
        if (success)
        {
            m_systemProfileFreshness = ProfileFreshness::Unknown;
            checkSystemAuthStatus();
        }
        auto callback = std::move(*continuationState);
        if (callback)
            callback(success);
    };
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [complete](int exitCode, QProcess::ExitStatus exitStatus) mutable
            { complete(exitStatus == QProcess::NormalExit && exitCode == 0); });
    connect(process, &QProcess::errorOccurred, this, [complete](QProcess::ProcessError) mutable { complete(false); });
    process->start(QStringLiteral("pkexec"),
                   {helper, QStringLiteral("--delete-profile"), QStringLiteral("--uid"), uidText});
}

void EnrollmentSession::cancel()
{
    const bool profileMutation =
        m_systemAuthBusy || m_commitAfterSystemRevoke || m_pendingOperation == PendingOperation::Delete ||
        m_pendingOperation == PendingOperation::Reset || m_pendingOperation == PendingOperation::Commit;
    ++m_profileMutationEpoch;
    m_commitAfterSystemRevoke = false;
    m_sessionTimer.stop();
    if (m_requestActive)
    {
        m_requestActive = false;
        m_activeGeneration = 0;
        m_pendingOperation = PendingOperation::None;
        m_worker->cancel();
    }
    m_keyProvider->cancel();
    if (std::exchange(m_keyStoredDuringEnrollment, false))
        m_keyProvider->deleteKey([](KWalletKeyProvider::Result result) { result.clear(); });
    clearSensitive();
    if (m_state != State::Idle && m_state != State::Complete)
        setState(
            State::Cancelled,
            profileMutation
                ? translate(
                      "The profile operation was cancelled. Any completed system-profile revocation remains in effect.")
                : translate("Enrollment was cancelled. No partial profile was saved."));
}

void EnrollmentSession::deleteProfile()
{
    if (m_requestActive || busy() || m_systemAuthBusy)
        return;
    m_pendingOperation = PendingOperation::Delete;
    setState(State::Saving, translate("Revoking the system profile before deleting local data…"));
    const quint64 epoch = m_profileMutationEpoch;
    revokeSystemProfileThen(
        [this, epoch](bool success)
        {
            if (epoch != m_profileMutationEpoch)
                return;
            if (!success)
            {
                m_pendingOperation = PendingOperation::None;
                fail(QStringLiteral("system-profile-revoke-failed"),
                     translate("The system profile could not be revoked. The local profile was not deleted."));
                return;
            }
            deleteLocalProfile(epoch);
        });
}

void EnrollmentSession::deleteLocalProfile(quint64 epoch)
{
    setState(State::OpeningWallet, translate("Opening your user-session wallet…"));
    m_keyProvider->requestKey(
        [this, epoch](KWalletKeyProvider::Result result)
        {
            if (epoch != m_profileMutationEpoch)
            {
                result.clear();
                return;
            }
            if (result.state != KWalletKeyProvider::State::Available)
            {
                result.clear();
                fail(QStringLiteral("vault-key-unavailable"), translate("The profile key is locked or unavailable."));
                return;
            }
            const quint64 generation = nextGeneration();
            QByteArray request =
                IdentityProtocol::keyRequest(IdentityProtocol::Operation::DeleteProfile, generation, result.key);
            result.clear();
            m_pendingOperation = PendingOperation::Delete;
            m_requestActive = true;
            m_activeGeneration = generation;
            setState(State::Saving, translate("Deleting the encrypted face profile…"));
            m_worker->execute(generation, std::move(request),
                              [this](quint64 completed, QByteArrayView payload, const QString &error)
                              { handleResponse(completed, payload, error); });
        });
}

void EnrollmentSession::resetUnreadable()
{
    if (m_requestActive || busy() || m_systemAuthBusy)
        return;
    m_pendingOperation = PendingOperation::Reset;
    setState(State::Saving, translate("Revoking the system profile before resetting local data…"));
    const quint64 epoch = m_profileMutationEpoch;
    revokeSystemProfileThen(
        [this, epoch](bool success)
        {
            if (epoch != m_profileMutationEpoch)
                return;
            if (!success)
            {
                m_pendingOperation = PendingOperation::None;
                fail(QStringLiteral("system-profile-revoke-failed"),
                     translate("The system profile could not be revoked. The local profile was not reset."));
                return;
            }
            const quint64 generation = nextGeneration();
            m_pendingOperation = PendingOperation::Reset;
            m_requestActive = true;
            m_activeGeneration = generation;
            setState(State::Saving, translate("Resetting unreadable local profile data…"));
            m_worker->execute(generation, IdentityProtocol::resetRequest(generation),
                              [this](quint64 completed, QByteArrayView payload, const QString &error)
                              { handleResponse(completed, payload, error); });
        });
}

void EnrollmentSession::setPageActive(bool active)
{
    m_pageActive = active;
    if (!active)
        cancel();
    else
        refreshProfileStatus();
    Q_EMIT stateChanged();
}

void EnrollmentSession::handleResponse(quint64 generation, QByteArrayView payload, const QString &transportError)
{
    if (generation == 0 || generation != m_activeGeneration)
        return;
    m_requestActive = false;
    m_activeGeneration = 0;
    if (!transportError.isEmpty())
    {
        m_pendingOperation = PendingOperation::None;
        fail(transportError, transportError == QLatin1String("cancelled")
                                 ? translate("The identity operation was cancelled.")
                                 : translate("The local identity worker was unavailable."));
        return;
    }
    IdentityProtocol::Response response;
    QString parseError;
    if (!IdentityProtocol::parseResponse(payload, generation, &response, &parseError))
    {
        m_pendingOperation = PendingOperation::None;
        fail(parseError, translate("The local identity worker returned an invalid response."));
        return;
    }
    if (response.kind == IdentityProtocol::ResponseKind::Error)
    {
        const QString code = QStringLiteral("identity-error-%1").arg(response.code);
        const QString guidance =
            response.code == 7    ? translate("No face was found. Center one face and retry.")
            : response.code == 8  ? translate("More than one face was found. Retry with one face.")
            : response.code == 9  ? translate("Lighting or image quality is unsuitable. Adjust and retry.")
            : response.code == 10 ? translate("Move the face away from the frame edge and retry.")
            : response.code == 11
                ? translate("This sample is too similar to an existing sample. Change appearance or pose.")
            : response.code == 23 ? translate("The experimental image check rejected this sample. Adjust lighting and "
                                              "retry; this is not verified attack detection.")
                                  : translate("The local identity operation failed safely.");
        response.clearSensitive();
        if (m_pendingOperation == PendingOperation::Capture &&
            ((response.code >= 7 && response.code <= 11) || response.code == 23))
        {
            m_pendingOperation = PendingOperation::None;
            handleSampleError(code, guidance);
            return;
        }
        m_pendingOperation = PendingOperation::None;
        fail(code, guidance);
        return;
    }

    switch (m_pendingOperation)
    {
    case PendingOperation::Status:
        if (response.kind != IdentityProtocol::ResponseKind::Status)
        {
            fail(QStringLiteral("identity-protocol-error"), translate("Profile status was unavailable."));
            break;
        }
        m_storedSampleCount =
            response.sensitivePayload.isEmpty() ? 0 : static_cast<quint8>(response.sensitivePayload.at(0));
        m_profileState = response.code == 0   ? ProfileState::Absent
                         : response.code == 1 ? ProfileState::Ready
                         : response.code == 2 ? ProfileState::Unreadable
                         : response.code == 3 ? ProfileState::ModelMismatch
                                              : ProfileState::Unavailable;
        Q_EMIT profileChanged();
        checkSystemAuthStatus();
        break;
    case PendingOperation::Capture:
    {
        if (response.kind != IdentityProtocol::ResponseKind::Sample ||
            response.sensitivePayload.size() != IdentityProtocol::EmbeddingBytes)
        {
            fail(QStringLiteral("identity-protocol-error"), translate("The enrollment sample was invalid."));
            break;
        }
        m_embeddings.append(response.sensitivePayload);
        const int capturedSampleIndex = m_sampleCount;
        const bool automatic = std::exchange(m_captureAutomatic, false);
        ++m_sampleCount;
        Q_EMIT samplesChanged();
        Q_EMIT guidePhaseChanged();
        Q_EMIT sampleCaptured(capturedSampleIndex, automatic);
        setState(m_sampleCount >= minimumSamples() ? State::ReadyToSave : State::Enrolling,
                 m_sampleCount >= minimumSamples()
                     ? translate("Minimum enrollment is complete. Five samples are recommended; eight is the limit.")
                     : translate("Sample accepted. Capture the next deliberate sample."));
        break;
    }
    case PendingOperation::Commit:
        if (response.kind != IdentityProtocol::ResponseKind::Ack)
        {
            fail(QStringLiteral("identity-protocol-error"), translate("The profile was not saved."));
            break;
        }
        m_sessionTimer.stop();
        m_storedSampleCount = m_sampleCount;
        m_profileState = ProfileState::Ready;
        m_keyStoredDuringEnrollment = false;
        clearSensitive();
        setState(State::Complete, translate("Your encrypted local face profile was saved."));
        Q_EMIT profileChanged();
        checkSystemAuthStatus();
        break;
    case PendingOperation::Delete:
    case PendingOperation::Reset:
        if (response.kind != IdentityProtocol::ResponseKind::Ack)
        {
            fail(QStringLiteral("identity-protocol-error"), translate("The profile data was not deleted."));
            break;
        }
        m_keyProvider->deleteKey(
            [this](KWalletKeyProvider::Result result)
            {
                const bool removed = result.state == KWalletKeyProvider::State::Absent;
                result.clear();
                if (!removed)
                {
                    m_profileState = ProfileState::Unavailable;
                    m_storedSampleCount = 0;
                    fail(QStringLiteral("vault-key-delete-failed"),
                         translate("The encrypted profile was deleted, but its KWallet key could not be removed."));
                    Q_EMIT profileChanged();
                    checkSystemAuthStatus();
                    return;
                }
                m_profileState = ProfileState::Absent;
                m_storedSampleCount = 0;
                setState(State::Complete, translate("The local face profile was deleted."));
                Q_EMIT profileChanged();
                checkSystemAuthStatus();
            });
        break;
    case PendingOperation::None:
        break;
    }
    response.clearSensitive();
    m_pendingOperation = PendingOperation::None;
}

void EnrollmentSession::fail(const QString &code, const QString &text)
{
    m_sessionTimer.stop();
    m_pendingOperation = PendingOperation::None;
    clearSensitive();
    if (std::exchange(m_keyStoredDuringEnrollment, false))
    {
        m_keyProvider->deleteKey(
            [this, code, text](KWalletKeyProvider::Result result)
            {
                const bool rolledBack = result.state == KWalletKeyProvider::State::Absent;
                result.clear();
                m_errorCode = rolledBack ? code : QStringLiteral("vault-key-rollback-failed");
                setState(State::Failed,
                         rolledBack
                             ? text
                             : translate("Enrollment failed and the unused KWallet key could not be rolled back."));
            });
        return;
    }
    m_errorCode = code;
    setState(State::Failed, text);
}

void EnrollmentSession::handleSampleError(const QString &code, const QString &text)
{
    m_captureAutomatic = false;
    m_errorCode = code;
    m_state = m_sampleCount >= minimumSamples() ? State::ReadyToSave : State::Enrolling;
    m_statusText = text;
    Q_EMIT stateChanged();
}

void EnrollmentSession::setState(State state, const QString &text)
{
    m_state = state;
    m_statusText = text;
    if (state != State::Failed)
        m_errorCode.clear();
    Q_EMIT stateChanged();
    Q_EMIT guidePhaseChanged();
}

void EnrollmentSession::clearSensitive()
{
    m_key.fill(0);
    m_key.clear();
    m_embeddings.fill(0);
    m_embeddings.clear();
    m_sampleCount = 0;
    m_keyNeedsStore = false;
    m_captureAutomatic = false;
    Q_EMIT samplesChanged();
    Q_EMIT guidePhaseChanged();
}

quint64 EnrollmentSession::nextGeneration()
{
    ++m_generation;
    if (m_generation == 0)
        ++m_generation;
    return m_generation;
}
