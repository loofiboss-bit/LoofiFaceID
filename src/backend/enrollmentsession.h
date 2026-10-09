// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QTimer>
#include <QVariantList>

#include <functional>

class CameraPreviewSession;
class IdentityWorkerClient;
class KWalletKeyProvider;

class EnrollmentSession final : public QObject
{
    Q_OBJECT

    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(ProfileState profileState READ profileState NOTIFY profileChanged)
    Q_PROPERTY(int sampleCount READ sampleCount NOTIFY samplesChanged)
    Q_PROPERTY(int storedSampleCount READ storedSampleCount NOTIFY profileChanged)
    Q_PROPERTY(int minimumSamples READ minimumSamples CONSTANT)
    Q_PROPERTY(int recommendedSamples READ recommendedSamples CONSTANT)
    Q_PROPERTY(int maximumSamples READ maximumSamples CONSTANT)
    Q_PROPERTY(int remainingSeconds READ remainingSeconds NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool enrollmentActive READ enrollmentActive NOTIFY stateChanged)
    Q_PROPERTY(bool canStartEnrollment READ canStartEnrollment NOTIFY stateChanged)
    Q_PROPERTY(bool canCancel READ canCancel NOTIFY stateChanged)
    Q_PROPERTY(bool canCapture READ canCapture NOTIFY stateChanged)
    Q_PROPERTY(bool canFinish READ canFinish NOTIFY stateChanged)
    Q_PROPERTY(bool enrollmentComplete READ enrollmentComplete NOTIFY stateChanged)
    Q_PROPERTY(bool profileReady READ profileReady NOTIFY profileChanged)
    Q_PROPERTY(bool replacementConfirmationRequired READ replacementConfirmationRequired NOTIFY profileChanged)
    Q_PROPERTY(bool profileNeedsAttention READ profileNeedsAttention NOTIFY profileChanged)
    Q_PROPERTY(QString profileStatusText READ profileStatusText NOTIFY profileChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QString errorCode READ errorCode NOTIFY stateChanged)
    Q_PROPERTY(GuidePhase guidePhase READ guidePhase NOTIFY guidePhaseChanged)
    Q_PROPERTY(AuthTargetStatus sddmAuthStatus READ sddmAuthStatus NOTIFY systemAuthChanged)
    Q_PROPERTY(bool sddmAuthConfigured READ sddmAuthConfigured NOTIFY systemAuthChanged)
    Q_PROPERTY(bool sddmAuthEnabled READ sddmAuthEnabled NOTIFY systemAuthChanged)
    Q_PROPERTY(bool sddmAuthCanEnable READ sddmAuthCanEnable NOTIFY systemAuthChanged)
    Q_PROPERTY(QString sddmAuthMode READ sddmAuthMode NOTIFY systemAuthChanged)
    Q_PROPERTY(QString sddmIntegrationStatusText READ sddmIntegrationStatusText NOTIFY systemAuthChanged)
    Q_PROPERTY(QString sddmAuthStatusText READ sddmAuthStatusText NOTIFY systemAuthChanged)
    Q_PROPERTY(QString sddmAuthErrorCode READ sddmAuthErrorCode NOTIFY systemAuthChanged)
    Q_PROPERTY(AuthTargetStatus plasmaLockAuthStatus READ plasmaLockAuthStatus NOTIFY systemAuthChanged)
    Q_PROPERTY(bool plasmaLockAuthConfigured READ plasmaLockAuthConfigured NOTIFY systemAuthChanged)
    Q_PROPERTY(bool plasmaLockAuthEnabled READ plasmaLockAuthEnabled NOTIFY systemAuthChanged)
    Q_PROPERTY(bool plasmaLockAuthCanEnable READ plasmaLockAuthCanEnable NOTIFY systemAuthChanged)
    Q_PROPERTY(QString plasmaLockAuthMode READ plasmaLockAuthMode NOTIFY systemAuthChanged)
    Q_PROPERTY(QString plasmaLockIntegrationStatusText READ plasmaLockIntegrationStatusText NOTIFY systemAuthChanged)
    Q_PROPERTY(QString plasmaLockAuthStatusText READ plasmaLockAuthStatusText NOTIFY systemAuthChanged)
    Q_PROPERTY(QString plasmaLockAuthErrorCode READ plasmaLockAuthErrorCode NOTIFY systemAuthChanged)
    Q_PROPERTY(ProfileFreshness systemProfileFreshness READ systemProfileFreshness NOTIFY systemAuthChanged)
    Q_PROPERTY(QString systemProfileFreshnessText READ systemProfileFreshnessText NOTIFY systemAuthChanged)
    Q_PROPERTY(bool systemAuthBusy READ systemAuthBusy NOTIFY systemAuthChanged)
    Q_PROPERTY(bool systemProfileCanSync READ systemProfileCanSync NOTIFY systemAuthChanged)
    Q_PROPERTY(QVariantList sddmReadiness READ sddmReadiness NOTIFY systemAuthChanged)
    Q_PROPERTY(QVariantList plasmaLockReadiness READ plasmaLockReadiness NOTIFY systemAuthChanged)
    Q_PROPERTY(AuthOperationState authOperationState READ authOperationState NOTIFY systemAuthChanged)
    Q_PROPERTY(AuthOperationResult authOperationResult READ authOperationResult NOTIFY systemAuthChanged)
    Q_PROPERTY(QString authOperationTarget READ authOperationTarget NOTIFY systemAuthChanged)
    Q_PROPERTY(QString authOperationText READ authOperationText NOTIFY systemAuthChanged)
    Q_PROPERTY(QString authOperationErrorCode READ authOperationErrorCode NOTIFY systemAuthChanged)

  public:
    enum class State
    {
        Idle,
        OpeningWallet,
        Enrolling,
        Capturing,
        ReadyToSave,
        Saving,
        Complete,
        Failed,
        Cancelled,
    };
    Q_ENUM(State)

    enum class ProfileState
    {
        Unknown,
        Checking,
        Absent,
        Ready,
        Unreadable,
        ModelMismatch,
        VaultLocked,
        Unavailable,
    };
    Q_ENUM(ProfileState)

    enum class GuidePhase
    {
        Camera,
        Frontal,
        Left,
        Right,
        Tilt,
        Natural,
        Review,
        Saving,
        Complete,
        Error,
    };
    Q_ENUM(GuidePhase)

    enum class ProfileFreshness
    {
        Unknown,
        Current,
        Stale,
    };
    Q_ENUM(ProfileFreshness)

    enum class AuthTargetStatus
    {
        MissingComponents,
        Off,
        Ready,
        Enabled,
        Blocked,
    };
    Q_ENUM(AuthTargetStatus)

    enum class Readiness
    {
        Unknown,
        Missing,
        Available,
        Incompatible
    };
    Q_ENUM(Readiness)
    enum class AuthOperationState
    {
        Idle,
        OpeningWallet,
        Updating,
        Checking
    };
    Q_ENUM(AuthOperationState)
    enum class AuthOperationResult
    {
        None,
        Success,
        Cancelled,
        WalletUnavailable,
        NotReady,
        Failed,
        ReadbackFailed
    };
    Q_ENUM(AuthOperationResult)

    struct AuthStatusSnapshot
    {
        ProfileFreshness freshness = ProfileFreshness::Unknown;
        Readiness components = Readiness::Unknown;
        Readiness sddmApi = Readiness::Unknown;
        Readiness sddmTheme = Readiness::Unknown;
        Readiness sddmPam = Readiness::Unknown;
        Readiness plasmaApi = Readiness::Unknown;
        Readiness plasmaPam = Readiness::Unknown;
        Readiness daemon = Readiness::Unknown;
        Readiness systemProfile = Readiness::Unknown;
        QString sddmMode = QStringLiteral("unknown");
        QString plasmaMode = QStringLiteral("unknown");
    };
    using AuthStatusCompletion = std::function<void(AuthStatusSnapshot)>;
    using AuthStatusProbe = std::function<void(AuthStatusCompletion)>;
    using AuthOperationCompletion = std::function<void(int exitCode, bool normalExit)>;
    using AuthOperationRunner = std::function<void(QStringList arguments, QByteArray input, AuthOperationCompletion)>;

    EnrollmentSession(CameraPreviewSession *preview, IdentityWorkerClient *worker, KWalletKeyProvider *keyProvider,
                      QObject *parent = nullptr);
    EnrollmentSession(CameraPreviewSession *preview, IdentityWorkerClient *worker, KWalletKeyProvider *keyProvider,
                      AuthStatusProbe statusProbe, AuthOperationRunner operationRunner, QObject *parent = nullptr);
    ~EnrollmentSession() override;

    [[nodiscard]] State state() const;
    [[nodiscard]] ProfileState profileState() const;
    [[nodiscard]] int sampleCount() const;
    [[nodiscard]] int storedSampleCount() const;
    [[nodiscard]] int minimumSamples() const;
    [[nodiscard]] int recommendedSamples() const;
    [[nodiscard]] int maximumSamples() const;
    [[nodiscard]] int remainingSeconds() const;
    [[nodiscard]] bool busy() const;
    [[nodiscard]] bool enrollmentActive() const;
    [[nodiscard]] bool canStartEnrollment() const;
    [[nodiscard]] bool canCancel() const;
    [[nodiscard]] bool canCapture() const;
    [[nodiscard]] bool canFinish() const;
    [[nodiscard]] bool enrollmentComplete() const;
    [[nodiscard]] bool profileReady() const;
    [[nodiscard]] bool replacementConfirmationRequired() const;
    [[nodiscard]] bool profileNeedsAttention() const;
    [[nodiscard]] QString profileStatusText() const;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QString errorCode() const;
    [[nodiscard]] GuidePhase guidePhase() const;
    [[nodiscard]] AuthTargetStatus sddmAuthStatus() const;
    [[nodiscard]] bool sddmAuthConfigured() const;
    [[nodiscard]] bool sddmAuthEnabled() const;
    [[nodiscard]] bool sddmAuthCanEnable() const;
    [[nodiscard]] QString sddmAuthMode() const;
    [[nodiscard]] QString sddmIntegrationStatusText() const;
    [[nodiscard]] QString sddmAuthStatusText() const;
    [[nodiscard]] QString sddmAuthErrorCode() const;
    [[nodiscard]] AuthTargetStatus plasmaLockAuthStatus() const;
    [[nodiscard]] bool plasmaLockAuthConfigured() const;
    [[nodiscard]] bool plasmaLockAuthEnabled() const;
    [[nodiscard]] bool plasmaLockAuthCanEnable() const;
    [[nodiscard]] QString plasmaLockAuthMode() const;
    [[nodiscard]] QString plasmaLockIntegrationStatusText() const;
    [[nodiscard]] QString plasmaLockAuthStatusText() const;
    [[nodiscard]] QString plasmaLockAuthErrorCode() const;
    [[nodiscard]] ProfileFreshness systemProfileFreshness() const;
    [[nodiscard]] QString systemProfileFreshnessCode() const;
    [[nodiscard]] QString systemProfileFreshnessText() const;
    [[nodiscard]] bool systemAuthBusy() const;
    [[nodiscard]] bool systemProfileCanSync() const;
    [[nodiscard]] QVariantList sddmReadiness() const;
    [[nodiscard]] QVariantList plasmaLockReadiness() const;
    [[nodiscard]] AuthOperationState authOperationState() const;
    [[nodiscard]] AuthOperationResult authOperationResult() const;
    [[nodiscard]] QString authOperationTarget() const;
    [[nodiscard]] QString authOperationText() const;
    [[nodiscard]] QString authOperationErrorCode() const;

    Q_INVOKABLE void refreshProfileStatus();
    Q_INVOKABLE void checkSystemAuthStatus();
    Q_INVOKABLE void syncSystemProfile();
    Q_INVOKABLE void enableSddmAuth();
    Q_INVOKABLE void disableSddmAuth();
    Q_INVOKABLE void enablePlasmaLockAuth();
    Q_INVOKABLE void disablePlasmaLockAuth();
    Q_INVOKABLE void setSddmAuthMode(const QString &mode);
    Q_INVOKABLE void setPlasmaLockAuthMode(const QString &mode);
    Q_INVOKABLE void startEnrollment();
    Q_INVOKABLE void captureSample(bool automatic = false);
    Q_INVOKABLE void discardLastSample();
    Q_INVOKABLE void finishAndSave();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void deleteProfile();
    Q_INVOKABLE void resetUnreadable();
    Q_INVOKABLE void setPageActive(bool active);

  Q_SIGNALS:
    void stateChanged();
    void profileChanged();
    void samplesChanged();
    void guidePhaseChanged();
    void sampleCaptured(int sampleIndex, bool automatic);
    void systemAuthChanged();

  private:
    void runStatus(const QByteArray &key);
    void commitEnrollment();
    void handleResponse(quint64 generation, QByteArrayView payload, const QString &transportError);
    void handleSampleError(const QString &code, const QString &text);
    void fail(const QString &code, const QString &text);
    void setState(State state, const QString &text);
    void clearSensitive();
    [[nodiscard]] quint64 nextGeneration();
    void updateAuthTargetStatus(const QString &target, const QString &mode, bool authComponentsInstalled,
                                bool integrationAvailable, bool pamServiceAvailable, bool systemProfileReady,
                                bool daemonReady);
    void runAuthTargetOperation(const QString &target, const QString &mode, bool resync = false);
    void finishAuthTargetOperation(int exitCode, bool normalExit);
    void finishAuthReadback();
    void applyAuthStatusSnapshot(const AuthStatusSnapshot &snapshot);
    [[nodiscard]] QVariantList authReadinessRows(bool sddm) const;
    void refreshAuthTargetStatus(const QString &target);
    void revokeSystemProfileThen(std::function<void(bool)> continuation);
    void deleteLocalProfile(quint64 epoch);

    enum class PendingOperation
    {
        None,
        Status,
        Capture,
        Commit,
        Delete,
        Reset,
    };

    CameraPreviewSession *m_preview = nullptr;
    IdentityWorkerClient *m_worker = nullptr;
    KWalletKeyProvider *m_keyProvider = nullptr;
    State m_state = State::Idle;
    ProfileState m_profileState = ProfileState::Unknown;
    PendingOperation m_pendingOperation = PendingOperation::None;
    QByteArray m_key;
    QByteArray m_embeddings;
    QString m_statusText;
    QString m_errorCode;
    quint64 m_generation = 0;
    quint64 m_activeGeneration = 0;
    int m_sampleCount = 0;
    int m_storedSampleCount = 0;
    int m_remainingSeconds = 0;
    bool m_requestActive = false;
    bool m_pageActive = false;
    bool m_keyNeedsStore = false;
    bool m_keyStoredDuringEnrollment = false;
    bool m_captureAutomatic = false;
    AuthTargetStatus m_sddmAuthStatus = AuthTargetStatus::Blocked;
    AuthTargetStatus m_plasmaLockAuthStatus = AuthTargetStatus::Blocked;
    bool m_sddmAuthConfigured = false;
    bool m_plasmaLockAuthConfigured = false;
    QString m_sddmAuthErrorCode = QStringLiteral("status-not-checked");
    QString m_plasmaLockAuthErrorCode = QStringLiteral("status-not-checked");
    QString m_sddmAuthMode = QStringLiteral("unknown");
    QString m_plasmaLockAuthMode = QStringLiteral("unknown");
    QString m_sddmIntegrationStatus = QStringLiteral("not-checked");
    QString m_plasmaLockIntegrationStatus = QStringLiteral("not-checked");
    ProfileFreshness m_systemProfileFreshness = ProfileFreshness::Unknown;
    bool m_authComponentsAvailable = false;
    bool m_sddmIntegrationAvailable = false;
    bool m_plasmaLockIntegrationAvailable = false;
    bool m_daemonReady = false;
    bool m_systemProfileReady = false;
    bool m_authPolicyStatusReady = false;
    int m_authPolicyQueriesRemaining = 0;
    quint64 m_authPolicyQueryGeneration = 0;
    bool m_systemAuthBusy = false;
    AuthStatusSnapshot m_authReadiness;
    AuthStatusProbe m_authStatusProbe;
    AuthOperationRunner m_authOperationRunner;
    AuthOperationState m_authOperationState = AuthOperationState::Idle;
    AuthOperationResult m_authOperationResult = AuthOperationResult::None;
    QString m_authOperationTarget;
    QString m_expectedSddmMode;
    QString m_expectedPlasmaMode;
    bool m_authOperationRequiresProfile = false;
    bool m_authOperationRequiresFreshness = false;
    quint64 m_authOperationGeneration = 0;
    QTimer m_authReadbackTimer;
    QProcess *m_systemProfileMutationProcess = nullptr;
    bool m_commitAfterSystemRevoke = false;
    quint64 m_profileMutationEpoch = 0;
    QTimer m_sessionTimer;
};
