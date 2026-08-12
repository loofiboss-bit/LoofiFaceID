// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QString>

class CameraPreviewSession;
class IdentityWorkerClient;
class KWalletKeyProvider;

class LocalVerificationSession final : public QObject
{
    Q_OBJECT

    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(Result result READ result NOTIFY resultChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool canVerify READ canVerify NOTIFY stateChanged)
    Q_PROPERTY(bool hasResult READ hasResult NOTIFY resultChanged)
    Q_PROPERTY(bool canClearResult READ canClearResult NOTIFY resultChanged)
    Q_PROPERTY(bool isMatch READ isMatch NOTIFY resultChanged)
    Q_PROPERTY(bool isNoMatch READ isNoMatch NOTIFY resultChanged)
    Q_PROPERTY(bool isAmbiguous READ isAmbiguous NOTIFY resultChanged)
    Q_PROPERTY(bool isUnavailable READ isUnavailable NOTIFY resultChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QString errorCode READ errorCode NOTIFY stateChanged)

  public:
    enum class State
    {
        Idle,
        OpeningWallet,
        Verifying,
        Complete,
        Failed,
        Cancelled,
        RateLimited,
    };
    Q_ENUM(State)

    enum class Result
    {
        None,
        Match,
        NoMatch,
        Ambiguous,
        NoProfile,
        VaultLocked,
        ModelMismatch,
        Unavailable,
        Cancelled,
        InternalFailure,
    };
    Q_ENUM(Result)

    LocalVerificationSession(CameraPreviewSession *preview, IdentityWorkerClient *worker,
                             KWalletKeyProvider *keyProvider, QObject *parent = nullptr);
    ~LocalVerificationSession() override;

    [[nodiscard]] State state() const;
    [[nodiscard]] Result result() const;
    [[nodiscard]] bool busy() const;
    [[nodiscard]] bool canVerify() const;
    [[nodiscard]] bool hasResult() const;
    [[nodiscard]] bool canClearResult() const;
    [[nodiscard]] bool isMatch() const;
    [[nodiscard]] bool isNoMatch() const;
    [[nodiscard]] bool isAmbiguous() const;
    [[nodiscard]] bool isUnavailable() const;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QString errorCode() const;

    Q_INVOKABLE void verifyCurrentFrame();
    Q_INVOKABLE void clearResult();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void setPageActive(bool active);

  Q_SIGNALS:
    void stateChanged();
    void resultChanged();

  private:
    void handleResponse(quint64 generation, QByteArrayView payload, const QString &transportError);
    void setResult(Result result, State state, const QString &text, const QString &error = {});
    [[nodiscard]] quint64 nextGeneration();

    CameraPreviewSession *m_preview = nullptr;
    IdentityWorkerClient *m_worker = nullptr;
    KWalletKeyProvider *m_keyProvider = nullptr;
    State m_state = State::Idle;
    Result m_result = Result::None;
    QString m_statusText;
    QString m_errorCode;
    quint64 m_generation = 0;
    quint64 m_activeGeneration = 0;
    bool m_requestActive = false;
    bool m_pageActive = false;
    QElapsedTimer m_rateLimit;
};
