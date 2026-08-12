// SPDX-License-Identifier: GPL-3.0-or-later

#include "localverificationsession.h"

#include "camerapreviewsession.h"
#include "identityprotocol.h"
#include "identityworkerclient.h"
#include "kwalletkeyprovider.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QImage>

namespace
{
QString translate(const char *text)
{
    return QCoreApplication::translate("LocalVerificationSession", text);
}

constexpr qint64 MinimumIntervalMs = 2000;
} // namespace

LocalVerificationSession::LocalVerificationSession(CameraPreviewSession *preview, IdentityWorkerClient *worker,
                                                   KWalletKeyProvider *keyProvider, QObject *parent)
    : QObject(parent), m_preview(preview), m_worker(worker), m_keyProvider(keyProvider),
      m_statusText(translate("Start preview, then explicitly test one current frame."))
{
    Q_ASSERT(m_preview);
    Q_ASSERT(m_worker);
    Q_ASSERT(m_keyProvider);
    connect(m_preview, &CameraPreviewSession::stateChanged, this,
            [this]()
            {
                if (m_preview->state() != CameraPreviewSession::State::Streaming)
                    cancel();
                else
                    Q_EMIT stateChanged();
            });
    connect(qGuiApp, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState state)
            {
                if (state != Qt::ApplicationActive)
                    cancel();
            });
}

LocalVerificationSession::~LocalVerificationSession()
{
    cancel();
}

LocalVerificationSession::State LocalVerificationSession::state() const
{
    return m_state;
}

LocalVerificationSession::Result LocalVerificationSession::result() const
{
    return m_result;
}

bool LocalVerificationSession::busy() const
{
    return m_state == State::OpeningWallet || m_state == State::Verifying;
}

bool LocalVerificationSession::canVerify() const
{
    return m_pageActive && !m_worker->busy() && !busy() &&
           m_preview->state() == CameraPreviewSession::State::Streaming && m_preview->frameAvailable();
}

bool LocalVerificationSession::hasResult() const
{
    return m_result != Result::None;
}

bool LocalVerificationSession::canClearResult() const
{
    return !busy() && hasResult();
}

bool LocalVerificationSession::isMatch() const
{
    return m_result == Result::Match;
}

bool LocalVerificationSession::isNoMatch() const
{
    return m_result == Result::NoMatch;
}

bool LocalVerificationSession::isAmbiguous() const
{
    return m_result == Result::Ambiguous;
}

bool LocalVerificationSession::isUnavailable() const
{
    return m_result == Result::NoProfile || m_result == Result::VaultLocked || m_result == Result::ModelMismatch ||
           m_result == Result::Unavailable || m_result == Result::Cancelled || m_result == Result::InternalFailure;
}

QString LocalVerificationSession::statusText() const
{
    return m_statusText;
}

QString LocalVerificationSession::errorCode() const
{
    return m_errorCode;
}

void LocalVerificationSession::verifyCurrentFrame()
{
    if (!canVerify())
        return;
    if (m_rateLimit.isValid() && m_rateLimit.elapsed() < MinimumIntervalMs)
    {
        setResult(Result::Unavailable, State::RateLimited,
                  translate("Please wait briefly before requesting another local test."),
                  QStringLiteral("rate-limited"));
        return;
    }
    QImage frame;
    if (!m_preview->copyCurrentFrame(&frame))
    {
        setResult(Result::Unavailable, State::Failed, translate("No current preview frame is available."),
                  QStringLiteral("frame-unavailable"));
        return;
    }
    m_result = Result::None;
    Q_EMIT resultChanged();
    m_state = State::OpeningWallet;
    m_statusText = translate("Opening your user-session wallet…");
    m_errorCode.clear();
    Q_EMIT stateChanged();
    m_keyProvider->requestKey(
        [this, frame = std::move(frame)](KWalletKeyProvider::Result keyResult) mutable
        {
            if (keyResult.state != KWalletKeyProvider::State::Available)
            {
                frame.fill(0);
                const bool locked = keyResult.state == KWalletKeyProvider::State::Locked ||
                                    keyResult.state == KWalletKeyProvider::State::Cancelled;
                keyResult.clear();
                setResult(locked ? Result::VaultLocked
                                 : (keyResult.state == KWalletKeyProvider::State::Absent ? Result::NoProfile
                                                                                         : Result::Unavailable),
                          State::Failed,
                          locked ? translate("The user-session vault is locked or cancelled.")
                                 : translate("No usable encrypted face profile is available."),
                          locked ? QStringLiteral("vault-locked") : QStringLiteral("profile-unavailable"));
                return;
            }
            const quint64 generation = nextGeneration();
            QString error;
            QByteArray request = IdentityProtocol::verifyRequest(generation, keyResult.key, frame, &error);
            keyResult.clear();
            frame.fill(0);
            if (request.isEmpty())
            {
                setResult(Result::InternalFailure, State::Failed,
                          translate("The current frame could not be prepared safely."), error);
                return;
            }
            m_requestActive = true;
            m_activeGeneration = generation;
            m_state = State::Verifying;
            m_statusText = translate("Comparing one frame locally…");
            m_errorCode.clear();
            Q_EMIT stateChanged();
            m_worker->execute(generation, std::move(request),
                              [this](quint64 completed, QByteArrayView payload, const QString &transportError)
                              { handleResponse(completed, payload, transportError); });
        });
}

void LocalVerificationSession::clearResult()
{
    if (busy())
        return;
    setResult(Result::None, State::Idle, translate("Start preview, then explicitly test one current frame."));
}

void LocalVerificationSession::cancel()
{
    const bool wasBusy = busy();
    if (m_requestActive)
    {
        m_requestActive = false;
        m_activeGeneration = 0;
        m_worker->cancel();
    }
    m_keyProvider->cancel();
    if (wasBusy)
        setResult(Result::Cancelled, State::Cancelled, translate("The local recognition test was cancelled."));
    else if (m_result != Result::None)
        setResult(Result::None, State::Idle, translate("Start preview, then explicitly test one current frame."));
}

void LocalVerificationSession::setPageActive(bool active)
{
    m_pageActive = active;
    if (!active)
        cancel();
    Q_EMIT stateChanged();
}

void LocalVerificationSession::handleResponse(quint64 generation, QByteArrayView payload, const QString &transportError)
{
    if (generation == 0 || generation != m_activeGeneration)
        return;
    m_requestActive = false;
    m_activeGeneration = 0;
    m_rateLimit.restart();
    if (!transportError.isEmpty())
    {
        setResult(transportError == QLatin1String("cancelled") ? Result::Cancelled : Result::Unavailable,
                  transportError == QLatin1String("cancelled") ? State::Cancelled : State::Failed,
                  transportError == QLatin1String("cancelled")
                      ? translate("The local recognition test was cancelled.")
                      : translate("The local identity worker was unavailable."),
                  transportError);
        return;
    }
    IdentityProtocol::Response response;
    QString error;
    if (!IdentityProtocol::parseResponse(payload, generation, &response, &error))
    {
        setResult(Result::InternalFailure, State::Failed,
                  translate("The local identity worker returned an invalid response."), error);
        return;
    }
    if (response.kind == IdentityProtocol::ResponseKind::Verification)
    {
        const Result result =
            response.code == 1 ? Result::Match : (response.code == 2 ? Result::NoMatch : Result::Ambiguous);
        const QString text =
            result == Result::Match
                ? translate("Match — experimental local comparison only. Nothing was unlocked or authorized.")
            : result == Result::NoMatch
                ? translate("No match — the current frame did not meet the provisional local policy.")
                : translate("Ambiguous — the local comparison is too close to the provisional boundary.");
        response.clearSensitive();
        setResult(result, State::Complete, text);
        return;
    }
    if (response.kind == IdentityProtocol::ResponseKind::Error)
    {
        const Result result = response.code == 13   ? Result::NoProfile
                              : response.code == 14 ? Result::VaultLocked
                              : response.code == 16 ? Result::ModelMismatch
                              : response.code == 18 ? Result::Cancelled
                              : response.code == 20 ? Result::Unavailable
                                                    : Result::InternalFailure;
        response.clearSensitive();
        setResult(result, result == Result::Cancelled ? State::Cancelled : State::Failed,
                  result == Result::NoProfile     ? translate("No enrolled face profile exists.")
                  : result == Result::VaultLocked ? translate("The user-session vault is locked.")
                  : result == Result::ModelMismatch
                      ? translate("The enrolled profile belongs to a different model version.")
                  : result == Result::Cancelled ? translate("The local recognition test was cancelled.")
                                                : translate("The local recognition test failed safely."),
                  QStringLiteral("identity-error-%1").arg(response.code));
        return;
    }
    response.clearSensitive();
    setResult(Result::InternalFailure, State::Failed, translate("The local identity response was invalid."),
              QStringLiteral("identity-protocol-error"));
}

void LocalVerificationSession::setResult(Result result, State state, const QString &text, const QString &error)
{
    m_result = result;
    m_state = state;
    m_statusText = text;
    m_errorCode = error;
    Q_EMIT resultChanged();
    Q_EMIT stateChanged();
}

quint64 LocalVerificationSession::nextGeneration()
{
    ++m_generation;
    if (m_generation == 0)
        ++m_generation;
    return m_generation;
}
