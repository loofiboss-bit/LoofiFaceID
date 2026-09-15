// SPDX-License-Identifier: GPL-3.0-or-later

#include "visionanalysissession.h"

#include "camerapreviewsession.h"
#include "previewprotocol.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QImage>
#include <QProcess>
#include <QStringList>
#include <QtEndian>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

#ifndef KFACEAUTH_VISION_WORKER_PATH
#define KFACEAUTH_VISION_WORKER_PATH "/usr/libexec/kfaceauth-vision-worker"
#endif

#ifndef KFACEAUTH_MODEL_ROOT
#define KFACEAUTH_MODEL_ROOT "/usr/share/kfaceauth/models"
#endif

namespace
{
constexpr quint16 ProtocolVersion = 2;
constexpr quint8 AnalyzeOperation = 1;
constexpr quint8 Rgb8PixelFormat = 1;
constexpr quint8 SuccessResponse = 0x81;
constexpr quint8 ErrorResponse = 0xff;
constexpr quint8 KnownQualityFlags = 0x0f;
constexpr quint32 InferenceTimeoutMs = 2000;
constexpr qsizetype RequestHeaderBytes = 24;
constexpr qsizetype MaxRequestBytes =
    static_cast<qsizetype>(PreviewProtocol::MaxWidth) * PreviewProtocol::MaxHeight * 4 + RequestHeaderBytes;
constexpr qsizetype SuccessHeaderBytes = 16;
constexpr qsizetype FaceRectangleBytes = 8;
constexpr qsizetype LandmarkBytes = 20;
constexpr qsizetype FaceObservationBytes = FaceRectangleBytes + LandmarkBytes;
constexpr qsizetype MaxResponseBytes = SuccessHeaderBytes + 8 * FaceObservationBytes;
constexpr int GuidanceIntervalMs = 250;

QString translate(const char *text)
{
    return QCoreApplication::translate("VisionAnalysisSession", text);
}

void appendU16(QByteArray *bytes, quint16 value)
{
    const quint16 encoded = qToBigEndian(value);
    bytes->append(reinterpret_cast<const char *>(&encoded), sizeof(encoded));
}

void appendU32(QByteArray *bytes, quint32 value)
{
    const quint32 encoded = qToBigEndian(value);
    bytes->append(reinterpret_cast<const char *>(&encoded), sizeof(encoded));
}

void appendU64(QByteArray *bytes, quint64 value)
{
    const quint64 encoded = qToBigEndian(value);
    bytes->append(reinterpret_cast<const char *>(&encoded), sizeof(encoded));
}

quint16 readU16(QByteArrayView bytes, qsizetype offset)
{
    return qFromBigEndian<quint16>(reinterpret_cast<const uchar *>(bytes.data() + offset));
}

quint32 readU32(QByteArrayView bytes, qsizetype offset)
{
    return qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(bytes.data() + offset));
}

quint64 readU64(QByteArrayView bytes, qsizetype offset)
{
    return qFromBigEndian<quint64>(reinterpret_cast<const uchar *>(bytes.data() + offset));
}

} // namespace

VisionAnalysisSession::VisionAnalysisSession(CameraPreviewSession *previewSession, QObject *parent)
    : VisionAnalysisSession(previewSession, QStringLiteral(KFACEAUTH_VISION_WORKER_PATH), parent)
{
}

VisionAnalysisSession::VisionAnalysisSession(CameraPreviewSession *previewSession, QString workerPath, QObject *parent)
    : VisionAnalysisSession(previewSession, std::move(workerPath), QProcessEnvironment(), parent)
{
}

VisionAnalysisSession::VisionAnalysisSession(CameraPreviewSession *previewSession, QString workerPath,
                                             QProcessEnvironment workerEnvironment, QObject *parent)
    : QObject(parent), m_previewSession(previewSession), m_workerPath(std::move(workerPath)),
      m_workerEnvironment(std::move(workerEnvironment)),
      m_statusText(translate("Analyze one current preview frame when you choose."))
{
    Q_ASSERT(m_previewSession);
    m_startupTimer.setSingleShot(true);
    m_startupTimer.setInterval(2000);
    m_inferenceTimer.setSingleShot(true);
    m_inferenceTimer.setInterval(InferenceTimeoutMs);
    m_shutdownTimer.setSingleShot(true);
    m_shutdownTimer.setInterval(1000);
    m_trackingTimer.setInterval(GuidanceIntervalMs);
    m_trackingTimer.setSingleShot(false);

    connect(&m_startupTimer, &QTimer::timeout, this, [this]() { fail(QStringLiteral("startup-timeout")); });
    connect(&m_inferenceTimer, &QTimer::timeout, this, [this]() { fail(QStringLiteral("inference-timeout")); });
    connect(&m_shutdownTimer, &QTimer::timeout, this, [this]() { fail(QStringLiteral("shutdown-timeout")); });
    connect(&m_trackingTimer, &QTimer::timeout, this,
            [this]()
            {
                if (m_continuousTracking && canAnalyze() && !m_requestInFlight)
                    analyzeCurrentFrame();
            });
    connect(m_previewSession, &CameraPreviewSession::stateChanged, this,
            [this]()
            {
                Q_EMIT availabilityChanged();
                if (m_previewSession->state() != CameraPreviewSession::State::Streaming)
                    cancelForLifecycle();
            });
    connect(m_previewSession, &CameraPreviewSession::frameChanged, this,
            [this]()
            {
                Q_EMIT availabilityChanged();
                if (!m_previewSession->frameAvailable())
                    cancelForLifecycle();
            });
    connect(m_previewSession, &QObject::destroyed, this,
            [this]()
            {
                m_previewSession = nullptr;
                cancelForLifecycle();
                Q_EMIT availabilityChanged();
            });
    if (auto *application = qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
    {
        connect(application, &QGuiApplication::applicationStateChanged, this,
                [this](Qt::ApplicationState state)
                {
                    if (state != Qt::ApplicationActive)
                        cancelForLifecycle();
                });
    }
}

VisionAnalysisSession::~VisionAnalysisSession()
{
    ++m_generation;
    m_trackingTimer.stop();
    m_requestInFlight = false;
    m_workerStarted = false;
    m_ignoringProcessExit = true;
    if (m_process)
    {
        m_process->disconnect(this);
        m_process->kill();
        m_process->setParent(QCoreApplication::instance());
        connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), m_process, &QObject::deleteLater);
        m_process = nullptr;
    }
    clearSensitiveData();
    clearResult();
}

VisionAnalysisSession::State VisionAnalysisSession::state() const
{
    return m_state;
}

bool VisionAnalysisSession::busy() const
{
    return m_state == State::Starting || m_state == State::Analyzing;
}

bool VisionAnalysisSession::canAnalyze() const
{
    return m_previewSession && m_previewSession->state() == CameraPreviewSession::State::Streaming &&
           m_previewSession->frameAvailable();
}

bool VisionAnalysisSession::resultAvailable() const
{
    return m_state == State::Complete && m_faceFinding != FaceFinding::Unknown;
}

int VisionAnalysisSession::frameWidth() const
{
    return m_frameWidth;
}

int VisionAnalysisSession::frameHeight() const
{
    return m_frameHeight;
}

bool VisionAnalysisSession::hasFace() const
{
    return resultAvailable() && m_faceFinding == FaceFinding::OneFace;
}

bool VisionAnalysisSession::noFace() const
{
    return resultAvailable() && m_faceFinding == FaceFinding::NoFace;
}

bool VisionAnalysisSession::multipleFaces() const
{
    return resultAvailable() && m_faceFinding == FaceFinding::MultipleFaces;
}

bool VisionAnalysisSession::framingSuitable() const
{
    return hasFace() && m_position == Position::Centered && m_distance == Distance::Suitable &&
           m_brightness == Quality::Suitable && m_contrast == Quality::Suitable && m_sharpness == Quality::Suitable;
}

bool VisionAnalysisSession::faceDetected() const
{
    return hasFace();
}

QRectF VisionAnalysisSession::faceRect() const
{
    return m_faceRect;
}

bool VisionAnalysisSession::continuousTracking() const
{
    return m_continuousTracking;
}

void VisionAnalysisSession::setContinuousTracking(bool enabled)
{
    if (m_continuousTracking == enabled)
    {
        if (!enabled && (m_state != State::Idle || m_process))
            cancelAnalysis();
        return;
    }
    m_continuousTracking = enabled;
    if (m_continuousTracking)
        m_trackingTimer.start();
    else
        m_trackingTimer.stop();
    Q_EMIT continuousTrackingChanged();
    if (m_continuousTracking && canAnalyze() && !m_requestInFlight)
        analyzeCurrentFrame();
    else if (!m_continuousTracking && (m_state != State::Idle || m_process))
        cancelAnalysis();
}

void VisionAnalysisSession::startGuidance()
{
    setContinuousTracking(true);
}

void VisionAnalysisSession::stopGuidance()
{
    setContinuousTracking(false);
}

VisionAnalysisSession::GuidanceState VisionAnalysisSession::guidanceState() const
{
    return m_guidanceState;
}

VisionAnalysisSession::Pose VisionAnalysisSession::detectedPose() const
{
    return m_detectedPose;
}

bool VisionAnalysisSession::poseMatches(int sampleIndex) const
{
    if (!hasFace() || sampleIndex < 0 || sampleIndex > 4)
        return false;
    switch (sampleIndex)
    {
    case 0:
        return m_detectedPose == Pose::Frontal;
    case 1:
        return m_detectedPose == Pose::Left;
    case 2:
        return m_detectedPose == Pose::Right;
    case 3:
        return m_detectedPose == Pose::Tilt;
    case 4:
        return m_detectedPose == Pose::Natural || m_detectedPose == Pose::Frontal;
    default:
        return false;
    }
}

VisionAnalysisSession::FaceFinding VisionAnalysisSession::faceFinding() const
{
    return m_faceFinding;
}

int VisionAnalysisSession::faceCount() const
{
    return m_faceCount;
}

VisionAnalysisSession::Position VisionAnalysisSession::position() const
{
    return m_position;
}

VisionAnalysisSession::Distance VisionAnalysisSession::distance() const
{
    return m_distance;
}

VisionAnalysisSession::Quality VisionAnalysisSession::brightness() const
{
    return m_brightness;
}

VisionAnalysisSession::Quality VisionAnalysisSession::contrast() const
{
    return m_contrast;
}

VisionAnalysisSession::Quality VisionAnalysisSession::sharpness() const
{
    return m_sharpness;
}

QString VisionAnalysisSession::statusText() const
{
    return m_statusText;
}

QString VisionAnalysisSession::resultSummary() const
{
    if (noFace())
        return translate("No face was found in this frame.");
    if (multipleFaces())
        return translate("Multiple faces were found in this frame.");
    if (hasFace())
        return translate("One face was found in this frame.");
    return {};
}

QString VisionAnalysisSession::guidanceText() const
{
    if (m_guidanceState == GuidanceState::NoFace)
        return translate("Place one face in the camera frame.");
    if (m_guidanceState == GuidanceState::MultipleFaces)
        return translate("Only one face may be visible.");
    if (m_position == Position::OffCenter)
        return translate("Center your face in the frame.");
    if (m_distance == Distance::TooFar)
        return translate("Move closer to the camera.");
    if (m_distance == Distance::TooClose)
        return translate("Move farther from the camera.");
    if (m_brightness == Quality::Low)
        return translate("Add more even light.");
    if (m_brightness == Quality::High)
        return translate("Reduce bright light on your face.");
    if (m_contrast == Quality::Low)
        return translate("Use more even front lighting.");
    if (m_sharpness == Quality::Low)
        return translate("Hold still and check that the camera lens is clear.");
    if (m_guidanceState == GuidanceState::Blurred)
        return translate("Hold still and clean the camera lens.");
    if (m_guidanceState == GuidanceState::WrongPose)
        return translate("Adjust your pose to match the highlighted step.");
    if (framingSuitable())
        return translate("Face framing and image quality are ready.");
    if (!hasFace())
        return translate("Place one face in the camera frame.");
    return translate("Adjust your position and lighting, then try one frame again.");
}

QString VisionAnalysisSession::errorCode() const
{
    return m_errorCode;
}

quint64 VisionAnalysisSession::generation() const
{
    return m_generation;
}

void VisionAnalysisSession::analyzeCurrentFrame()
{
    if (!canAnalyze())
        return;
    if (m_requestInFlight)
    {
        if (!m_sessionMode)
            stopAnalysis(true);
        return;
    }
    if (busy())
        return;
    if (m_process && (!m_sessionMode || !m_workerStarted))
    {
        stopAnalysis(true);
        return;
    }

    QImage frame;
    if (!m_previewSession->copyCurrentFrame(&frame))
        return;
    if (frame.width() <= 0 || frame.width() > PreviewProtocol::MaxWidth || frame.height() <= 0 ||
        frame.height() > PreviewProtocol::MaxHeight)
    {
        frame.fill(0);
        fail(QStringLiteral("invalid-frame"));
        return;
    }

    QImage rgb = frame.convertToFormat(QImage::Format_RGB888);
    frame.fill(0);
    const qsizetype stride = rgb.bytesPerLine();
    const qsizetype frameSize = stride * rgb.height();
    if (rgb.isNull() || stride < static_cast<qsizetype>(rgb.width()) * 3 ||
        stride > static_cast<qsizetype>(rgb.width()) * 4 || frameSize <= 0 ||
        frameSize > MaxRequestBytes - RequestHeaderBytes || frameSize > std::numeric_limits<quint32>::max())
    {
        if (!rgb.isNull())
            rgb.fill(0);
        fail(QStringLiteral("invalid-frame"));
        return;
    }

    ++m_generation;
    clearSensitiveData();
    if (!m_sessionMode)
        clearResult();
    m_requestWidth = static_cast<quint16>(rgb.width());
    m_requestHeight = static_cast<quint16>(rgb.height());
    m_frameBytes = QByteArray(reinterpret_cast<const char *>(rgb.constBits()), frameSize);
    rgb.fill(0);

    QByteArray payload;
    payload.reserve(RequestHeaderBytes + m_frameBytes.size());
    appendU16(&payload, ProtocolVersion);
    payload.append(static_cast<char>(AnalyzeOperation));
    payload.append(static_cast<char>(Rgb8PixelFormat));
    appendU64(&payload, m_generation);
    appendU32(&payload, InferenceTimeoutMs);
    appendU16(&payload, m_requestWidth);
    appendU16(&payload, m_requestHeight);
    appendU32(&payload, static_cast<quint32>(stride));
    payload.append(m_frameBytes);
    if (payload.size() > MaxRequestBytes)
    {
        payload.fill(0);
        fail(QStringLiteral("invalid-frame"));
        return;
    }

    QByteArray request;
    request.reserve(4 + payload.size());
    appendU32(&request, static_cast<quint32>(payload.size()));
    request.append(payload);
    payload.fill(0);
    m_requestInFlight = true;
    startWorker(std::move(request));
}

void VisionAnalysisSession::cancelAnalysis()
{
    stopAnalysis(false);
}

void VisionAnalysisSession::stopAnalysis(bool replacementRequested)
{
    ++m_generation;
    m_replacementRequested = replacementRequested;
    m_startupTimer.stop();
    m_inferenceTimer.stop();
    m_shutdownTimer.stop();
    m_trackingTimer.stop();
    m_requestInFlight = false;
    m_workerStarted = false;
    m_sessionMode = false;
    m_ignoringProcessExit = true;
    if (m_process)
        m_process->kill();
    clearSensitiveData();
    clearResult();
    setState(State::Idle, replacementRequested
                              ? translate("Replacing the previous analysis with the current frame…")
                              : translate("Analysis was cancelled and its in-memory data was cleared."));
}

void VisionAnalysisSession::startWorker(QByteArray request)
{
    if (m_process)
    {
        if (m_sessionMode && m_workerStarted)
            writeRequest(std::move(request));
        else
        {
            request.fill(0);
            fail(QStringLiteral("worker-busy"));
        }
        return;
    }

    m_sessionMode = m_continuousTracking;
    m_errorCode.clear();
    m_ignoringProcessExit = false;
    m_responseReceived = false;
    m_workerStarted = false;
    m_stderrBytes = 0;
    m_process = new QProcess(this);
    m_process->setProgram(m_workerPath);
    QStringList arguments = {QStringLiteral("--model-root"), QStringLiteral(KFACEAUTH_MODEL_ROOT)};
    if (m_sessionMode)
        arguments.append(QStringLiteral("--session"));
    m_process->setArguments(arguments);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);
    m_process->setProcessEnvironment(m_workerEnvironment);
    connect(m_process, &QProcess::started, this,
            [this, request = std::move(request)]() mutable
            {
                if (m_ignoringProcessExit || !m_process)
                {
                    request.fill(0);
                    return;
                }
                m_startupTimer.stop();
                m_workerStarted = true;
                writeRequest(std::move(request));
            });
    connect(m_process, &QProcess::readyReadStandardOutput, this, &VisionAnalysisSession::readResponse);
    connect(m_process, &QProcess::readyReadStandardError, this,
            [this]()
            {
                if (!m_process)
                    return;
                QByteArray bytes = m_process->readAllStandardError();
                m_stderrBytes += bytes.size();
                bytes.fill(0);
                if (m_stderrBytes > 64 * 1024)
                    fail(QStringLiteral("protocol-error"));
            });
    connect(m_process, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error)
            {
                if (m_ignoringProcessExit)
                    return;
                if (error == QProcess::FailedToStart)
                {
                    QProcess *process = m_process;
                    m_process = nullptr;
                    if (process)
                    {
                        process->disconnect(this);
                        process->deleteLater();
                    }
                    fail(QStringLiteral("startup-failed"));
                }
                else if (error != QProcess::Crashed)
                    fail(QStringLiteral("worker-crashed"));
            });
    connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            &VisionAnalysisSession::processFinished);
    setState(State::Starting, translate("Starting local vision analysis…"));
    m_process->start();
    m_startupTimer.start();
}

void VisionAnalysisSession::writeRequest(QByteArray request)
{
    if (!m_process || !m_workerStarted)
    {
        request.fill(0);
        fail(QStringLiteral("protocol-error"));
        return;
    }
    setState(State::Analyzing, translate("Analyzing one in-memory frame…"));
    const qsizetype requestSize = request.size();
    const qint64 accepted = m_process->write(request);
    if (!m_sessionMode)
        m_process->closeWriteChannel();
    request.fill(0);
    clearSensitiveData();
    m_responseReceived = false;
    if (accepted != requestSize)
    {
        fail(QStringLiteral("protocol-error"));
        return;
    }
    m_inferenceTimer.start();
}

void VisionAnalysisSession::readResponse()
{
    if (!m_process)
        return;
    QByteArray bytes = m_process->readAllStandardOutput();
    if (m_responseReceived || m_responseBytes.size() + bytes.size() > MaxResponseBytes + 4)
    {
        bytes.fill(0);
        fail(QStringLiteral("protocol-error"));
        return;
    }
    m_responseBytes.append(bytes);
    bytes.fill(0);
    if (m_responseBytes.size() < 4)
        return;

    const quint32 payloadSize = readU32(QByteArrayView(m_responseBytes), 0);
    const bool isErrorPayload = payloadSize == 12;
    const bool isSuccessPayload = payloadSize >= SuccessHeaderBytes &&
                                  (payloadSize - SuccessHeaderBytes) % FaceObservationBytes == 0 &&
                                  (payloadSize - SuccessHeaderBytes) / FaceObservationBytes <= 8;
    if (payloadSize < 12 || payloadSize > MaxResponseBytes || (!isErrorPayload && !isSuccessPayload))
    {
        fail(QStringLiteral("protocol-error"));
        return;
    }
    if (m_responseBytes.size() < static_cast<qsizetype>(payloadSize) + 4)
        return;
    if (m_responseBytes.size() != static_cast<qsizetype>(payloadSize) + 4)
    {
        fail(QStringLiteral("protocol-error"));
        return;
    }

    Result result;
    QString responseError;
    const QByteArrayView payload(m_responseBytes.constData() + 4, payloadSize);
    if (!parseResponse(payload, &result, &responseError))
    {
        fail(responseError.isEmpty() ? QStringLiteral("protocol-error") : responseError);
        return;
    }
    m_inferenceTimer.stop();
    m_responseReceived = true;
    m_responseBytes.fill(0);
    m_responseBytes.clear();
    m_requestInFlight = false;
    if (m_sessionMode)
    {
        clearSensitiveData();
        setState(State::Complete, translate("Live guidance is ready. No image was saved."));
        applyResult(result);
        m_requestWidth = 0;
        m_requestHeight = 0;
        return;
    }
    m_pendingResult = result;
    m_statusText = translate("Finishing local analysis and clearing process data…");
    Q_EMIT stateChanged();
    m_shutdownTimer.start();
}

void VisionAnalysisSession::processFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    QProcess *process = m_process;
    m_process = nullptr;
    m_workerStarted = false;
    if (process)
        process->deleteLater();
    m_startupTimer.stop();
    m_inferenceTimer.stop();
    m_shutdownTimer.stop();

    if (m_ignoringProcessExit)
    {
        m_ignoringProcessExit = false;
        const bool replacementRequested = std::exchange(m_replacementRequested, false);
        Q_EMIT availabilityChanged();
        if (replacementRequested)
            QTimer::singleShot(0, this, &VisionAnalysisSession::analyzeCurrentFrame);
        return;
    }
    if (!m_responseReceived || !m_pendingResult || exitStatus != QProcess::NormalExit || exitCode != 0)
    {
        fail(QStringLiteral("worker-crashed"));
        return;
    }

    const Result result = *m_pendingResult;
    m_requestInFlight = false;
    clearSensitiveData();
    setState(State::Complete, translate("One-frame analysis is complete. No image was saved."));
    applyResult(result);
    m_requestWidth = 0;
    m_requestHeight = 0;
    if (m_continuousTracking && canAnalyze())
    {
        QTimer::singleShot(33, this,
                           [this]()
                           {
                               if (m_continuousTracking && canAnalyze() && m_state == State::Complete)
                               {
                                   m_state = State::Idle;
                                   analyzeCurrentFrame();
                               }
                           });
    }
}

void VisionAnalysisSession::fail(const QString &errorCode)
{
    m_startupTimer.stop();
    m_inferenceTimer.stop();
    m_shutdownTimer.stop();
    m_trackingTimer.stop();
    if (m_continuousTracking)
    {
        m_continuousTracking = false;
        Q_EMIT continuousTrackingChanged();
    }
    m_requestInFlight = false;
    m_workerStarted = false;
    m_sessionMode = false;
    m_ignoringProcessExit = true;
    m_replacementRequested = false;
    if (m_process)
        m_process->kill();
    clearSensitiveData();
    m_requestWidth = 0;
    m_requestHeight = 0;
    clearResult();
    m_state = State::Failed;
    m_errorCode = errorCode;
    m_statusText = textForError(errorCode);
    Q_EMIT stateChanged();
    Q_EMIT availabilityChanged();
}

void VisionAnalysisSession::setState(State state, const QString &statusText)
{
    m_state = state;
    m_statusText = statusText;
    if (state != State::Failed)
        m_errorCode.clear();
    Q_EMIT stateChanged();
    Q_EMIT availabilityChanged();
}

void VisionAnalysisSession::clearSensitiveData()
{
    if (!m_frameBytes.isEmpty())
        m_frameBytes.fill(0);
    if (!m_responseBytes.isEmpty())
        m_responseBytes.fill(0);
    m_frameBytes.clear();
    m_responseBytes.clear();
    m_pendingResult.reset();
    m_responseReceived = false;
}

void VisionAnalysisSession::clearResult()
{
    m_faceFinding = FaceFinding::Unknown;
    m_faceCount = -1;
    m_position = Position::Unknown;
    m_distance = Distance::Unknown;
    m_brightness = Quality::Unknown;
    m_contrast = Quality::Unknown;
    m_sharpness = Quality::Unknown;
    m_faceRect = {};
    m_frameWidth = 0;
    m_frameHeight = 0;
    m_guidanceState = GuidanceState::Unknown;
    m_detectedPose = Pose::Unknown;
    Q_EMIT resultChanged();
}

void VisionAnalysisSession::cancelForLifecycle()
{
    setContinuousTracking(false);
}

bool VisionAnalysisSession::parseResponse(QByteArrayView payload, Result *result, QString *errorCode) const
{
    if (!result || payload.size() < 12 || readU16(payload, 0) != ProtocolVersion)
        return false;
    const quint8 kind = static_cast<quint8>(payload.at(2));
    if (readU64(payload, 4) != m_generation)
    {
        if (errorCode)
            *errorCode = QStringLiteral("stale-response");
        return false;
    }
    if (kind == ErrorResponse)
    {
        if (payload.size() != 12 || static_cast<quint8>(payload.at(3)) == 0)
            return false;
        if (errorCode)
            *errorCode = QStringLiteral("analysis-error-%1").arg(static_cast<quint8>(payload.at(3)));
        return false;
    }
    if (kind != SuccessResponse || payload.size() < SuccessHeaderBytes)
        return false;

    const quint8 faceCount = static_cast<quint8>(payload.at(3));
    const bool withLandmarks = (payload.size() == SuccessHeaderBytes + faceCount * FaceObservationBytes);
    if (faceCount > 8 || !withLandmarks)
        return false;

    result->faceCount = faceCount;
    result->brightness = static_cast<quint8>(payload.at(12));
    result->contrast = static_cast<quint8>(payload.at(13));
    result->sharpness = static_cast<quint8>(payload.at(14));
    result->flags = static_cast<quint8>(payload.at(15));
    if ((result->flags & ~KnownQualityFlags) != 0 || (result->flags & 0x03) == 0x03)
        return false;
    if (m_requestWidth == 0 || m_requestHeight == 0)
        return false;
    result->frameWidth = m_requestWidth;
    result->frameHeight = m_requestHeight;
    const qsizetype stride = FaceObservationBytes;
    for (quint8 index = 0; index < faceCount; ++index)
    {
        const qsizetype offset = SuccessHeaderBytes + index * stride;
        const quint16 x = readU16(payload, offset);
        const quint16 y = readU16(payload, offset + 2);
        const quint16 width = readU16(payload, offset + 4);
        const quint16 height = readU16(payload, offset + 6);
        if (width == 0 || height == 0 || x + width > m_requestWidth || y + height > m_requestHeight)
            return false;
        if (index == 0)
        {
            result->x = x;
            result->y = y;
            result->width = width;
            result->height = height;
        }
        const qsizetype lmOffset = offset + FaceRectangleBytes;
        for (int lm = 0; lm < 5; ++lm)
        {
            const quint16 lx = readU16(payload, lmOffset + lm * 4);
            const quint16 ly = readU16(payload, lmOffset + lm * 4 + 2);
            if (lx >= m_requestWidth || ly >= m_requestHeight || lx < x || ly < y || lx >= x + width ||
                ly >= y + height)
                return false;
            if (index == 0)
                result->landmarks.append(QPointF(lx, ly));
        }
    }
    return true;
}

void VisionAnalysisSession::applyResult(const Result &result)
{
    m_faceCount = result.faceCount;
    m_faceFinding = result.faceCount == 0 ? FaceFinding::NoFace
                                          : (result.faceCount == 1 ? FaceFinding::OneFace : FaceFinding::MultipleFaces);
    m_brightness =
        (result.flags & 0x01) != 0 ? Quality::Low : ((result.flags & 0x02) != 0 ? Quality::High : Quality::Suitable);
    m_contrast = (result.flags & 0x04) != 0 ? Quality::Low : Quality::Suitable;
    m_sharpness = (result.flags & 0x08) != 0 ? Quality::Low : Quality::Suitable;
    m_position = Position::Unknown;
    m_distance = Distance::Unknown;
    m_faceRect = {};
    m_detectedPose = Pose::Unknown;
    m_frameWidth = result.frameWidth;
    m_frameHeight = result.frameHeight;

    if (result.faceCount == 0)
    {
        m_guidanceState = GuidanceState::NoFace;
        Q_EMIT resultChanged();
        return;
    }
    if (result.faceCount > 1)
    {
        m_guidanceState = GuidanceState::MultipleFaces;
        Q_EMIT resultChanged();
        return;
    }

    if (result.faceCount == 1)
    {
        m_faceRect = QRectF(result.x, result.y, result.width, result.height);
        const double centerX = result.x + result.width / 2.0;
        const double centerY = result.y + result.height / 2.0;
        const bool centered = centerX >= result.frameWidth * 0.35 && centerX <= result.frameWidth * 0.65 &&
                              centerY >= result.frameHeight * 0.30 && centerY <= result.frameHeight * 0.70;
        m_position = centered ? Position::Centered : Position::OffCenter;
        const double areaRatio =
            static_cast<double>(result.width) * result.height / (result.frameWidth * result.frameHeight);
        m_distance = areaRatio < 0.08 ? Distance::TooFar : (areaRatio > 0.55 ? Distance::TooClose : Distance::Suitable);

        if (result.landmarks.size() == 5)
        {
            const double eyeDistance = std::abs(result.landmarks.at(1).x() - result.landmarks.at(0).x());
            const double eyeMidX = (result.landmarks.at(0).x() + result.landmarks.at(1).x()) / 2.0;
            const double eyeMidY = (result.landmarks.at(0).y() + result.landmarks.at(1).y()) / 2.0;
            const double mouthMidY = (result.landmarks.at(3).y() + result.landmarks.at(4).y()) / 2.0;
            const double eyeToMouth = mouthMidY - eyeMidY;
            if (eyeDistance >= 1.0 && eyeToMouth >= 1.0)
            {
                const double yaw = (result.landmarks.at(2).x() - eyeMidX) / eyeDistance;
                const double pitch = (result.landmarks.at(2).y() - eyeMidY) / eyeToMouth - 0.5;
                if (std::abs(yaw) < 0.09 && std::abs(pitch) < 0.12)
                    m_detectedPose = Pose::Frontal;
                else if (yaw < -0.20)
                    m_detectedPose = Pose::Left;
                else if (yaw > 0.20)
                    m_detectedPose = Pose::Right;
                else if (std::abs(pitch) > 0.18)
                    m_detectedPose = Pose::Tilt;
                else
                    m_detectedPose = Pose::Natural;
            }
        }

        if (m_distance == Distance::TooFar)
            m_guidanceState = GuidanceState::TooFar;
        else if (m_distance == Distance::TooClose)
            m_guidanceState = GuidanceState::TooClose;
        else if (m_position == Position::OffCenter)
            m_guidanceState = GuidanceState::OffCenter;
        else if (m_brightness != Quality::Suitable || m_contrast == Quality::Low)
            m_guidanceState = GuidanceState::PoorLighting;
        else if (m_sharpness == Quality::Low)
            m_guidanceState = GuidanceState::Blurred;
        else if (m_detectedPose == Pose::Unknown)
            m_guidanceState = GuidanceState::WrongPose;
        else
            m_guidanceState = GuidanceState::Ready;
    }
    Q_EMIT resultChanged();
}

QString VisionAnalysisSession::textForError(const QString &errorCode) const
{
    if (errorCode == QLatin1String("startup-failed"))
        return translate("Local vision analysis could not be started.");
    if (errorCode == QLatin1String("startup-timeout"))
        return translate("Local vision analysis did not start in time.");
    if (errorCode == QLatin1String("inference-timeout"))
        return translate("The one-frame analysis timed out.");
    if (errorCode == QLatin1String("shutdown-timeout"))
        return translate("Local vision analysis did not stop cleanly.");
    if (errorCode == QLatin1String("worker-crashed"))
        return translate("Local vision analysis stopped unexpectedly.");
    if (errorCode == QLatin1String("stale-response"))
        return translate("An outdated analysis response was rejected.");
    if (errorCode == QLatin1String("invalid-frame"))
        return translate("The current preview frame could not be analyzed.");
    if (errorCode == QLatin1String("analysis-error-11"))
        return translate("YuNet is unavailable or failed model verification. Reinstall KFaceAuth and try again.");
    if (errorCode == QLatin1String("analysis-error-12"))
        return translate("The local YuNet detector returned invalid data. Try another frame or reinstall OpenCV.");
    if (errorCode.startsWith(QLatin1String("analysis-error-")))
        return translate("Local vision analysis could not process this frame.");
    return translate("Local vision analysis returned invalid data.");
}
