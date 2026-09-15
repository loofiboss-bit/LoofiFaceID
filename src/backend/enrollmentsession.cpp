// SPDX-License-Identifier: GPL-3.0-or-later

#include "enrollmentsession.h"

#include "camerapreviewsession.h"
#include "identityprotocol.h"
#include "identityworkerclient.h"
#include "kwalletkeyprovider.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QImage>

#include <algorithm>

namespace
{
QString translate(const char *text)
{
    return QCoreApplication::translate("EnrollmentSession", text);
}
} // namespace

EnrollmentSession::EnrollmentSession(CameraPreviewSession *preview, IdentityWorkerClient *worker,
                                     KWalletKeyProvider *keyProvider, QObject *parent)
    : QObject(parent), m_preview(preview), m_worker(worker), m_keyProvider(keyProvider),
      m_statusText(translate("Check your face profile or start a new enrollment."))
{
    Q_ASSERT(m_preview);
    Q_ASSERT(m_worker);
    Q_ASSERT(m_keyProvider);
    m_sessionTimer.setInterval(1000);
    connect(&m_sessionTimer, &QTimer::timeout, this,
            [this]()
            {
                if (m_remainingSeconds > 0)
                    --m_remainingSeconds;
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
    return m_remainingSeconds;
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
            m_remainingSeconds = 300;
            m_sessionTimer.start();
            setState(State::Enrolling, translate("Capture three to five deliberate appearance samples."));
        });
}

void EnrollmentSession::captureSample()
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

void EnrollmentSession::cancel()
{
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
        setState(State::Cancelled, translate("Enrollment was cancelled. No partial profile was saved."));
}

void EnrollmentSession::deleteProfile()
{
    if (m_requestActive || busy())
        return;
    setState(State::OpeningWallet, translate("Opening your user-session wallet…"));
    m_keyProvider->requestKey(
        [this](KWalletKeyProvider::Result result)
        {
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
    if (m_requestActive || busy())
        return;
    const quint64 generation = nextGeneration();
    m_pendingOperation = PendingOperation::Reset;
    m_requestActive = true;
    m_activeGeneration = generation;
    setState(State::Saving, translate("Resetting unreadable local profile data…"));
    m_worker->execute(generation, IdentityProtocol::resetRequest(generation),
                      [this](quint64 completed, QByteArrayView payload, const QString &error)
                      { handleResponse(completed, payload, error); });
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
                : translate("The local identity operation failed safely.");
        response.clearSensitive();
        if (m_pendingOperation == PendingOperation::Capture && response.code >= 7 && response.code <= 11)
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
        break;
    case PendingOperation::Capture:
        if (response.kind != IdentityProtocol::ResponseKind::Sample ||
            response.sensitivePayload.size() != IdentityProtocol::EmbeddingBytes)
        {
            fail(QStringLiteral("identity-protocol-error"), translate("The enrollment sample was invalid."));
            break;
        }
        m_embeddings.append(response.sensitivePayload);
        ++m_sampleCount;
        Q_EMIT samplesChanged();
        setState(m_sampleCount >= minimumSamples() ? State::ReadyToSave : State::Enrolling,
                 m_sampleCount >= minimumSamples()
                     ? translate("Minimum enrollment is complete. Five samples are recommended; eight is the limit.")
                     : translate("Sample accepted. Capture the next deliberate sample."));
        break;
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
                    return;
                }
                m_profileState = ProfileState::Absent;
                m_storedSampleCount = 0;
                setState(State::Complete, translate("The local face profile was deleted."));
                Q_EMIT profileChanged();
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
}

void EnrollmentSession::clearSensitive()
{
    m_key.fill(0);
    m_key.clear();
    m_embeddings.fill(0);
    m_embeddings.clear();
    m_sampleCount = 0;
    m_keyNeedsStore = false;
    Q_EMIT samplesChanged();
}

quint64 EnrollmentSession::nextGeneration()
{
    ++m_generation;
    if (m_generation == 0)
        ++m_generation;
    return m_generation;
}
