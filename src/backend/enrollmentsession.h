// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QTimer>

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
    Q_PROPERTY(bool profileNeedsAttention READ profileNeedsAttention NOTIFY profileChanged)
    Q_PROPERTY(QString profileStatusText READ profileStatusText NOTIFY profileChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QString errorCode READ errorCode NOTIFY stateChanged)
    Q_PROPERTY(GuidePhase guidePhase READ guidePhase NOTIFY guidePhaseChanged)

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

    EnrollmentSession(CameraPreviewSession *preview, IdentityWorkerClient *worker, KWalletKeyProvider *keyProvider,
                      QObject *parent = nullptr);
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
    [[nodiscard]] bool profileNeedsAttention() const;
    [[nodiscard]] QString profileStatusText() const;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QString errorCode() const;
    [[nodiscard]] GuidePhase guidePhase() const;

    Q_INVOKABLE void refreshProfileStatus();
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

  private:
    void runStatus(const QByteArray &key);
    void commitEnrollment();
    void handleResponse(quint64 generation, QByteArrayView payload, const QString &transportError);
    void handleSampleError(const QString &code, const QString &text);
    void fail(const QString &code, const QString &text);
    void setState(State state, const QString &text);
    void clearSensitive();
    [[nodiscard]] quint64 nextGeneration();

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
    QTimer m_sessionTimer;
};
