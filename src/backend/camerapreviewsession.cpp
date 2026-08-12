// SPDX-License-Identifier: GPL-3.0-or-later

#include "camerapreviewsession.h"

#include <QCborArray>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QProcessEnvironment>
#include <QSet>
#include <QUuid>

#include <algorithm>

#ifndef KFACEAUTH_PREVIEW_WORKER_PATH
#define KFACEAUTH_PREVIEW_WORKER_PATH "/usr/libexec/kfaceauth-camera-preview-worker"
#endif

namespace
{
QString translate(const char *text)
{
    return QCoreApplication::translate("CameraPreviewSession", text);
}
} // namespace

CameraPreviewSession::CameraPreviewSession(QObject *parent)
    : CameraPreviewSession(QStringLiteral(KFACEAUTH_PREVIEW_WORKER_PATH), parent)
{
}

CameraPreviewSession::CameraPreviewSession(QString workerPath, QObject *parent)
    : QAbstractListModel(parent), m_workerPath(std::move(workerPath))
{
    m_startupTimer.setSingleShot(true);
    m_startupTimer.setInterval(5000);
    m_stallTimer.setSingleShot(true);
    m_stallTimer.setInterval(3000);
    m_stopTimer.setSingleShot(true);
    m_stopTimer.setInterval(1000);
    m_countdownTimer.setInterval(1000);
    connect(&m_startupTimer, &QTimer::timeout, this,
            [this]()
            {
                if (m_state == State::Starting)
                    fail(QStringLiteral("startup-timeout"));
                else
                    fail(QStringLiteral("worker-crashed"));
                terminateWorker();
            });
    connect(&m_stallTimer, &QTimer::timeout, this,
            [this]()
            {
                fail(QStringLiteral("stream-stalled"));
                terminateWorker();
            });
    connect(&m_stopTimer, &QTimer::timeout, this,
            [this]()
            {
                fail(QStringLiteral("worker-crashed"));
                terminateWorker();
            });
    connect(&m_countdownTimer, &QTimer::timeout, this,
            [this]()
            {
                if (m_remainingSeconds > 0)
                    --m_remainingSeconds;
                Q_EMIT stateChanged();
                if (m_remainingSeconds == 0)
                    stopPreview();
            });
    if (auto *application = qobject_cast<QGuiApplication *>(QCoreApplication::instance()))
    {
        connect(application, &QGuiApplication::applicationStateChanged, this,
                [this](Qt::ApplicationState state)
                {
                    if (state != Qt::ApplicationActive && (m_state == State::Starting || m_state == State::Streaming))
                        stopPreview();
                });
    }
}

CameraPreviewSession::~CameraPreviewSession()
{
    m_expectedExit = true;
    if (m_process)
    {
        m_process->kill();
        m_process->setParent(QCoreApplication::instance());
        connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), m_process, &QObject::deleteLater);
        m_process = nullptr;
    }
    clearFrame();
}

int CameraPreviewSession::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : m_devices.size();
}

QVariant CameraPreviewSession::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_devices.size())
        return {};
    const Device &device = m_devices.at(index.row());
    if (role == LabelRole)
        return device.label;
    if (role == SpectrumRole)
        return device.spectrum;
    return {};
}

QHash<int, QByteArray> CameraPreviewSession::roleNames() const
{
    return {
        {LabelRole, QByteArrayLiteral("label")},
        {SpectrumRole, QByteArrayLiteral("spectrum")},
    };
}

CameraPreviewSession::State CameraPreviewSession::state() const
{
    return m_state;
}

bool CameraPreviewSession::busy() const
{
    return m_state == State::Discovering || m_state == State::Starting || m_state == State::Streaming ||
           m_state == State::Stopping;
}

bool CameraPreviewSession::previewActive() const
{
    return m_state == State::Streaming;
}

bool CameraPreviewSession::canStartPreview() const
{
    return m_state == State::Ready && m_selectedDeviceIndex >= 0 && m_selectedDeviceIndex < m_devices.size();
}

bool CameraPreviewSession::canStopPreview() const
{
    return m_state == State::Starting || m_state == State::Streaming;
}

bool CameraPreviewSession::canRefresh() const
{
    return !busy();
}

bool CameraPreviewSession::hasUsableCamera() const
{
    return !m_devices.isEmpty() && m_selectedDeviceIndex >= 0 && m_selectedDeviceIndex < m_devices.size();
}

int CameraPreviewSession::deviceCount() const
{
    return m_devices.size();
}

int CameraPreviewSession::selectedDeviceIndex() const
{
    return m_selectedDeviceIndex;
}

void CameraPreviewSession::setSelectedDeviceIndex(int index)
{
    const int bounded = index >= 0 && index < m_devices.size() ? index : -1;
    if (m_selectedDeviceIndex == bounded)
        return;
    m_selectedDeviceIndex = bounded;
    Q_EMIT selectionChanged();
    Q_EMIT stateChanged();
}

bool CameraPreviewSession::frameAvailable() const
{
    return !m_frame.isNull();
}

QString CameraPreviewSession::spectrum() const
{
    return m_spectrum;
}

int CameraPreviewSession::remainingSeconds() const
{
    return m_remainingSeconds;
}

quint64 CameraPreviewSession::droppedFrames() const
{
    return m_droppedFrames;
}

quint64 CameraPreviewSession::frameRevision() const
{
    return m_frameRevision;
}

QString CameraPreviewSession::statusText() const
{
    return m_statusText;
}

QString CameraPreviewSession::errorCode() const
{
    return m_errorCode;
}

QImage CameraPreviewSession::frame() const
{
    return m_frame;
}

bool CameraPreviewSession::copyCurrentFrame(QImage *destination) const
{
    if (!destination)
        return false;
    *destination = {};
    if (m_state != State::Streaming || m_frame.isNull() || m_frame.width() <= 0 ||
        m_frame.width() > PreviewProtocol::MaxWidth || m_frame.height() <= 0 ||
        m_frame.height() > PreviewProtocol::MaxHeight)
        return false;
    *destination = m_frame.copy();
    return !destination->isNull();
}

int CameraPreviewSession::deviceCountForSpectrum(const QString &spectrum) const
{
    return static_cast<int>(std::count_if(m_devices.cbegin(), m_devices.cend(),
                                          [&spectrum](const Device &device) { return device.spectrum == spectrum; }));
}

void CameraPreviewSession::refreshDevices()
{
    if (m_state == State::Starting || m_state == State::Streaming || m_state == State::Stopping)
        return;
    clearFrame();
    m_errorCode.clear();
    if (!m_process || m_process->state() == QProcess::NotRunning || m_expectedExit)
        startWorker();
    else
    {
        setState(State::Discovering, translate("Looking for local cameras…"));
        sendCommand(QStringLiteral("discover"));
        m_startupTimer.start();
    }
}

void CameraPreviewSession::startPreview()
{
    if (m_state != State::Ready || m_selectedDeviceIndex < 0 || m_selectedDeviceIndex >= m_devices.size())
        return;
    clearFrame();
    m_errorCode.clear();
    m_droppedFrames = 0;
    m_remainingSeconds = PreviewProtocol::MaxPreviewSeconds;
    setState(State::Starting, translate("Starting the selected camera…"));
    sendCommand(QStringLiteral("start"), m_devices.at(m_selectedDeviceIndex).token);
    m_startupTimer.start();
}

void CameraPreviewSession::stopPreview()
{
    if (m_state != State::Starting && m_state != State::Streaming)
    {
        clearFrame();
        return;
    }
    m_startupTimer.stop();
    m_stallTimer.stop();
    m_countdownTimer.stop();
    setState(State::Stopping, translate("Stopping and releasing the camera…"));
    sendCommand(QStringLiteral("stop"));
    m_stopTimer.start();
}

void CameraPreviewSession::clearFrame()
{
    if (!m_frame.isNull())
        m_frame.fill(0);
    m_frame = {};
    m_spectrum.clear();
    ++m_frameRevision;
    Q_EMIT frameChanged();
}

void CameraPreviewSession::startWorker()
{
    resetWorker(true);
    m_expectedExit = false;
    m_sessionId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    m_nextCommandSequence = 0;
    m_lastSequence = 0;
    m_stderrBytes = 0;
    m_parser.clear();
    m_process = new QProcess(this);
    m_process->setProgram(m_workerPath);
    m_process->setProcessChannelMode(QProcess::SeparateChannels);

    QProcessEnvironment environment;
    const QProcessEnvironment system = QProcessEnvironment::systemEnvironment();
    for (const QString &name :
         {QStringLiteral("LANG"), QStringLiteral("LC_ALL"), QStringLiteral("XDG_RUNTIME_DIR"),
          QStringLiteral("DBUS_SESSION_BUS_ADDRESS"), QStringLiteral("WAYLAND_DISPLAY"), QStringLiteral("DISPLAY")})
    {
        if (system.contains(name))
            environment.insert(name, system.value(name));
    }
    m_process->setProcessEnvironment(environment);
    connect(m_process, &QProcess::started, this,
            [this]()
            {
                sendCommand(QStringLiteral("discover"));
                m_startupTimer.start();
            });
    connect(m_process, &QProcess::readyReadStandardOutput, this, &CameraPreviewSession::processRecords);
    connect(m_process, &QProcess::readyReadStandardError, this,
            [this]()
            {
                m_stderrBytes += m_process->readAllStandardError().size();
                if (m_stderrBytes > 64 * 1024)
                {
                    fail(QStringLiteral("protocol-error"));
                    terminateWorker();
                }
            });
    connect(m_process, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError)
            {
                if (!m_expectedExit)
                    fail(QStringLiteral("worker-crashed"));
            });
    connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), this,
            [this](int, QProcess::ExitStatus)
            {
                const bool expected = m_expectedExit;
                if (m_process)
                    m_process->deleteLater();
                m_process = nullptr;
                if (!expected && m_state != State::Failed)
                    fail(QStringLiteral("worker-crashed"));
            });
    setState(State::Discovering, translate("Looking for local cameras…"));
    m_process->start();
    m_startupTimer.start();
}

void CameraPreviewSession::sendCommand(const QString &type, const QString &deviceToken)
{
    if (!m_process || m_process->state() != QProcess::Running)
        return;
    QCborMap command;
    command.insert(QStringLiteral("protocol"), PreviewProtocol::Version);
    command.insert(QStringLiteral("session"), m_sessionId);
    command.insert(QStringLiteral("sequence"), static_cast<qint64>(++m_nextCommandSequence));
    command.insert(QStringLiteral("type"), type);
    if (!deviceToken.isEmpty())
        command.insert(QStringLiteral("device"), deviceToken);
    const QByteArray encoded = PreviewProtocol::encode(command);
    if (encoded.isEmpty() || m_process->write(encoded) != encoded.size())
    {
        fail(QStringLiteral("protocol-error"));
        terminateWorker();
    }
}

void CameraPreviewSession::processRecords()
{
    QVector<QCborMap> records;
    QString errorCode;
    const QByteArray bytes = m_process->readAllStandardOutput();
    if (!m_parser.append(bytes, &records, &errorCode))
    {
        fail(errorCode);
        terminateWorker();
        return;
    }
    for (const QCborMap &record : records)
    {
        if (!handleRecord(record))
        {
            fail(QStringLiteral("protocol-error"));
            terminateWorker();
            return;
        }
    }
}

bool CameraPreviewSession::handleRecord(const QCborMap &record)
{
    const auto protocol = record.value(QStringLiteral("protocol"));
    const auto session = record.value(QStringLiteral("session"));
    const auto sequence = record.value(QStringLiteral("sequence"));
    const auto type = record.value(QStringLiteral("type"));
    if (!protocol.isInteger() || protocol.toInteger() != PreviewProtocol::Version || !session.isString() ||
        session.toString() != m_sessionId || !sequence.isInteger() || sequence.toInteger() <= 0 ||
        static_cast<quint64>(sequence.toInteger()) <= m_lastSequence || !type.isString())
        return false;
    m_lastSequence = sequence.toInteger();

    if (type.toString() == QLatin1String("devices"))
    {
        if (m_state != State::Discovering)
            return false;
        return handleDevices(record);
    }
    if (type.toString() == QLatin1String("started"))
    {
        const auto seconds = record.value(QStringLiteral("seconds"));
        if (m_state != State::Starting || !seconds.isInteger() ||
            seconds.toInteger() != PreviewProtocol::MaxPreviewSeconds)
            return false;
        m_startupTimer.stop();
        m_stallTimer.start();
        m_countdownTimer.start();
        setState(State::Streaming, translate("Preview is live and remains only in memory."));
        return true;
    }
    if (type.toString() == QLatin1String("frame"))
        return handleFrame(record);
    if (type.toString() == QLatin1String("stopped"))
    {
        if (m_state != State::Stopping && m_state != State::Streaming)
            return false;
        m_stopTimer.stop();
        m_startupTimer.stop();
        m_stallTimer.stop();
        m_countdownTimer.stop();
        m_remainingSeconds = 0;
        clearFrame();
        setState(State::Ready, translate("The camera has been released."));
        return true;
    }
    if (type.toString() == QLatin1String("error") && record.value(QStringLiteral("code")).isString())
    {
        const QString code = record.value(QStringLiteral("code")).toString();
        static const QSet<QString> allowedCodes = {
            QStringLiteral("permission-denied"),  QStringLiteral("no-camera"),
            QStringLiteral("camera-busy"),        QStringLiteral("camera-unavailable"),
            QStringLiteral("format-unavailable"), QStringLiteral("protocol-error"),
        };
        if (!allowedCodes.contains(code))
            return false;
        fail(code);
        return true;
    }
    return false;
}

bool CameraPreviewSession::handleDevices(const QCborMap &record)
{
    const auto value = record.value(QStringLiteral("devices"));
    if (!value.isArray() || value.toArray().size() > PreviewProtocol::MaxDevices)
        return false;
    QVector<Device> devices;
    QSet<QString> tokens;
    for (const QCborValue &itemValue : value.toArray())
    {
        if (!itemValue.isMap())
            return false;
        const QCborMap item = itemValue.toMap();
        const auto token = item.value(QStringLiteral("token"));
        const auto label = item.value(QStringLiteral("label"));
        const auto spectrum = item.value(QStringLiteral("spectrum"));
        if (!token.isString() || token.toString().isEmpty() || tokens.contains(token.toString()) || !label.isString() ||
            label.toString().toUtf8().size() > PreviewProtocol::MaxLabelBytes || !spectrum.isString() ||
            !QStringList{QStringLiteral("rgb"), QStringLiteral("ir"), QStringLiteral("unknown")}.contains(
                spectrum.toString()))
            return false;
        tokens.insert(token.toString());
        devices.push_back({token.toString(), label.toString(), spectrum.toString()});
    }
    m_startupTimer.stop();
    beginResetModel();
    m_devices = devices;
    endResetModel();
    m_selectedDeviceIndex = m_devices.isEmpty() ? -1 : 0;
    Q_EMIT devicesChanged();
    Q_EMIT selectionChanged();
    if (m_devices.isEmpty())
        fail(QStringLiteral("no-camera"));
    else
        setState(State::Ready, translate("Select a camera and start a private preview."));
    return true;
}

bool CameraPreviewSession::handleFrame(const QCborMap &record)
{
    if (m_state != State::Streaming)
        return false;
    const auto jpeg = record.value(QStringLiteral("jpeg"));
    const auto width = record.value(QStringLiteral("width"));
    const auto height = record.value(QStringLiteral("height"));
    const auto spectrum = record.value(QStringLiteral("spectrum"));
    const auto dropped = record.value(QStringLiteral("dropped"));
    if (!jpeg.isByteArray() || jpeg.toByteArray().isEmpty() ||
        jpeg.toByteArray().size() > PreviewProtocol::MaxJpegBytes || !width.isInteger() || width.toInteger() <= 0 ||
        width.toInteger() > PreviewProtocol::MaxWidth || !height.isInteger() || height.toInteger() <= 0 ||
        height.toInteger() > PreviewProtocol::MaxHeight || !spectrum.isString() ||
        !QStringList{QStringLiteral("rgb"), QStringLiteral("ir"), QStringLiteral("unknown")}.contains(
            spectrum.toString()) ||
        !dropped.isInteger() || dropped.toInteger() < 0)
        return false;
    const QImage frame = QImage::fromData(jpeg.toByteArray(), "JPEG");
    if (frame.isNull() || frame.width() != width.toInteger() || frame.height() != height.toInteger())
        return false;
    if (!m_frame.isNull())
        m_frame.fill(0);
    m_frame = frame;
    m_spectrum = spectrum.toString();
    m_droppedFrames = dropped.toInteger();
    ++m_frameRevision;
    m_stallTimer.start();
    Q_EMIT frameChanged();
    Q_EMIT stateChanged();
    return true;
}

void CameraPreviewSession::setState(State state, const QString &statusText)
{
    m_state = state;
    m_statusText = statusText;
    if (state != State::Failed)
        m_errorCode.clear();
    Q_EMIT stateChanged();
}

void CameraPreviewSession::fail(const QString &errorCode)
{
    m_startupTimer.stop();
    m_stallTimer.stop();
    m_stopTimer.stop();
    m_countdownTimer.stop();
    m_remainingSeconds = 0;
    clearFrame();
    m_state = State::Failed;
    m_errorCode = errorCode;
    m_statusText = textForError(errorCode);
    Q_EMIT stateChanged();
}

void CameraPreviewSession::terminateWorker()
{
    if (!m_process)
        return;
    m_expectedExit = true;
    m_process->kill();
}

void CameraPreviewSession::resetWorker(bool expected)
{
    if (!m_process)
        return;
    m_process->disconnect(this);
    m_expectedExit = expected;
    m_process->kill();
    m_process->deleteLater();
    m_process = nullptr;
}

QString CameraPreviewSession::textForError(const QString &errorCode) const
{
    if (errorCode == QLatin1String("permission-denied"))
        return translate("Camera permission was denied.");
    if (errorCode == QLatin1String("no-camera"))
        return translate("No compatible local camera was found.");
    if (errorCode == QLatin1String("camera-busy"))
        return translate("The selected camera is already in use.");
    if (errorCode == QLatin1String("format-unavailable"))
        return translate("The selected camera has no usable preview format.");
    if (errorCode == QLatin1String("startup-timeout"))
        return translate("The camera did not start in time.");
    if (errorCode == QLatin1String("stream-stalled"))
        return translate("The camera preview stopped delivering frames.");
    if (errorCode == QLatin1String("protocol-error"))
        return translate("The camera worker returned invalid data.");
    if (errorCode == QLatin1String("worker-crashed"))
        return translate("The camera worker stopped unexpectedly.");
    return translate("The local camera is unavailable.");
}
