// SPDX-License-Identifier: GPL-3.0-or-later

#include "previewworker.h"

#include <QCborArray>
#include <QCborMap>
#include <QCoreApplication>
#include <QSocketNotifier>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>

PreviewWorker::PreviewWorker(QObject *parent) : QObject(parent)
{
    m_previewLimit.setSingleShot(true);
    m_previewLimit.setInterval(PreviewProtocol::MaxPreviewSeconds * 1000);
    connect(&m_previewLimit, &QTimer::timeout, this, [this]() { stopPreview(QStringLiteral("time-limit")); });
    connect(&m_provider, &CameraProvider::started, this,
            [this]()
            {
                auto record = baseRecord(QStringLiteral("started"));
                record.insert(QStringLiteral("seconds"), PreviewProtocol::MaxPreviewSeconds);
                queueControl(record);
                m_previewLimit.start(
                    static_cast<int>(std::max<qint64>(1, m_deadlineMs - PreviewProtocol::monotonicMilliseconds())));
            });
    connect(&m_provider, &CameraProvider::rawFrameReady, this,
            [this](const QByteArray &rgb, int width, int height, const QString &spectrum)
            {
                if (m_sharedMemory.isAttached() && m_sharedMemory.size() >= rgb.size())
                {
                    if (m_sharedMemory.lock())
                    {
                        std::memcpy(m_sharedMemory.data(), rgb.constData(), rgb.size());
                        m_sharedMemory.unlock();
                        auto record = baseRecord(QStringLiteral("frame"));
                        record.insert(QStringLiteral("shm"), true);
                        record.insert(QStringLiteral("width"), width);
                        record.insert(QStringLiteral("height"), height);
                        record.insert(QStringLiteral("spectrum"), spectrum);
                        queueFrame(record);
                    }
                }
            });
    connect(&m_provider, &CameraProvider::frameReady, this,
            [this](const QByteArray &jpeg, int width, int height, const QString &spectrum)
            {
                if (m_sharedMemory.isAttached())
                    return;
                auto record = baseRecord(QStringLiteral("frame"));
                record.insert(QStringLiteral("jpeg"), jpeg);
                record.insert(QStringLiteral("width"), width);
                record.insert(QStringLiteral("height"), height);
                record.insert(QStringLiteral("spectrum"), spectrum);
                queueFrame(record);
            });
    connect(&m_provider, &CameraProvider::failed, this,
            [this](const QString &errorCode)
            {
                m_previewLimit.stop();
                m_provider.stop();
                m_previewInProgress = false;
                m_pendingFrame.clear();
                if (m_sharedMemory.isAttached())
                    m_sharedMemory.detach();
                sendError(errorCode);
                if (m_discoveryPending)
                    discover();
            });
    connect(&m_provider, &CameraProvider::deviceListChanged, this, [this]() { discover(); });
}

PreviewWorker::~PreviewWorker()
{
    m_provider.stop();
}

bool PreviewWorker::start()
{
    const int readFlags = ::fcntl(STDIN_FILENO, F_GETFL, 0);
    const int writeFlags = ::fcntl(STDOUT_FILENO, F_GETFL, 0);
    if (readFlags < 0 || writeFlags < 0 || ::fcntl(STDIN_FILENO, F_SETFL, readFlags | O_NONBLOCK) < 0 ||
        ::fcntl(STDOUT_FILENO, F_SETFL, writeFlags | O_NONBLOCK) < 0)
        return false;

    m_readNotifier = std::make_unique<QSocketNotifier>(STDIN_FILENO, QSocketNotifier::Read, this);
    m_writeNotifier = std::make_unique<QSocketNotifier>(STDOUT_FILENO, QSocketNotifier::Write, this);
    m_writeNotifier->setEnabled(false);
    connect(m_readNotifier.get(), &QSocketNotifier::activated, this, &PreviewWorker::readCommands);
    connect(m_writeNotifier.get(), &QSocketNotifier::activated, this, &PreviewWorker::flushOutput);
    return true;
}

void PreviewWorker::readCommands()
{
    char buffer[8192];
    for (;;)
    {
        const ssize_t count = ::read(STDIN_FILENO, buffer, sizeof(buffer));
        if (count > 0)
        {
            QVector<QCborMap> commands;
            QString errorCode;
            if (!m_parser.append(QByteArrayView(buffer, count), &commands, &errorCode))
            {
                sendError(errorCode);
                continue;
            }
            for (const QCborMap &command : commands)
                handleCommand(command);
            continue;
        }
        if (count == 0)
        {
            m_provider.stop();
            QCoreApplication::quit();
        }
        if (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
        {
            m_provider.stop();
            QCoreApplication::quit();
        }
        break;
    }
}

void PreviewWorker::handleCommand(const QCborMap &command)
{
    const auto version = command.value(QStringLiteral("protocol"));
    const auto session = command.value(QStringLiteral("session"));
    const auto sequence = command.value(QStringLiteral("sequence"));
    const auto type = command.value(QStringLiteral("type"));
    if (!version.isInteger() || version.toInteger() != PreviewProtocol::Version || !session.isString() ||
        session.toString().isEmpty() || session.toString().size() > 64 || !sequence.isInteger() ||
        sequence.toInteger() <= 0 || static_cast<quint64>(sequence.toInteger()) <= m_lastCommandSequence ||
        !type.isString())
    {
        if (m_sessionId.isEmpty() && session.isString() && !session.toString().isEmpty() &&
            session.toString().size() <= 64)
            m_sessionId = session.toString();
        sendError(QStringLiteral("protocol-error"));
        return;
    }
    if (m_sessionId.isEmpty())
        m_sessionId = session.toString();
    if (session.toString() != m_sessionId)
    {
        sendError(QStringLiteral("protocol-error"));
        return;
    }
    m_lastCommandSequence = sequence.toInteger();

    if (type.toString() == QLatin1String("discover") && command.size() == 4)
        discover();
    else if (type.toString() == QLatin1String("start") && command.size() == 6 &&
             command.value(QStringLiteral("deadline_ms")).isInteger() &&
             PreviewProtocol::validDeadline(command.value(QStringLiteral("deadline_ms")).toInteger(),
                                            PreviewProtocol::MaxPreviewSeconds) &&
             command.value(QStringLiteral("device")).isString() &&
             !command.value(QStringLiteral("device")).toString().isEmpty() &&
             command.value(QStringLiteral("device")).toString().size() <= 64)
        startPreview(command.value(QStringLiteral("device")).toString(),
                     command.value(QStringLiteral("deadline_ms")).toInteger());
    else if (type.toString() == QLatin1String("enrollment") && command.size() == 5 && m_provider.active() &&
             !m_enrollmentBudgetGranted && command.value(QStringLiteral("deadline_ms")).isInteger() &&
             m_deadlineMs > PreviewProtocol::monotonicMilliseconds() &&
             PreviewProtocol::validDeadline(command.value(QStringLiteral("deadline_ms")).toInteger(),
                                            PreviewProtocol::MaxEnrollmentSeconds))
    {
        m_enrollmentBudgetGranted = true;
        m_deadlineMs = command.value(QStringLiteral("deadline_ms")).toInteger();
        m_previewLimit.start(static_cast<int>(m_deadlineMs - PreviewProtocol::monotonicMilliseconds()));
        auto record = baseRecord(QStringLiteral("budget"));
        record.insert(QStringLiteral("deadline_ms"), m_deadlineMs);
        queueControl(record);
    }
    else if (type.toString() == QLatin1String("stop") && command.size() == 4)
        stopPreview(QStringLiteral("requested"));
    else
        sendError(QStringLiteral("protocol-error"));
}

void PreviewWorker::discover()
{
    if (m_sessionId.isEmpty() || m_previewInProgress)
    {
        m_discoveryPending = true;
        return;
    }
    m_discoveryPending = false;
    const QVector<CameraDescriptor> devices = m_provider.discover();
    QCborArray array;
    for (const CameraDescriptor &device : devices)
    {
        QCborMap item;
        item.insert(QStringLiteral("token"), device.token);
        item.insert(QStringLiteral("label"), device.label);
        item.insert(QStringLiteral("spectrum"), device.spectrum);
        array.append(item);
    }
    auto record = baseRecord(QStringLiteral("devices"));
    record.insert(QStringLiteral("devices"), array);
    queueControl(record);
}

void PreviewWorker::startPreview(const QString &token, qint64 deadlineMs)
{
    if (m_previewInProgress)
    {
        sendError(QStringLiteral("camera-busy"));
        return;
    }
    m_deadlineMs = deadlineMs;
    m_enrollmentBudgetGranted = false;
    m_droppedFrames = 0;
    m_pendingFrame.clear();
    if (!m_sessionId.isEmpty())
    {
        m_sharedMemory.setKey(QStringLiteral("kfaceauth_preview_%1").arg(m_sessionId));
        m_sharedMemory.attach();
    }
    QString errorCode;
    m_previewInProgress = true;
    if (!m_provider.start(token, &errorCode))
    {
        m_previewInProgress = false;
        if (m_sharedMemory.isAttached())
            m_sharedMemory.detach();
        sendError(errorCode);
        if (m_discoveryPending)
            discover();
    }
}

void PreviewWorker::stopPreview(const QString &reason)
{
    const bool wasActive = m_provider.active();
    m_previewLimit.stop();
    m_provider.stop();
    m_previewInProgress = false;
    m_pendingFrame.clear();
    if (m_sharedMemory.isAttached())
        m_sharedMemory.detach();
    auto record = baseRecord(QStringLiteral("stopped"));
    record.insert(QStringLiteral("reason"), reason);
    record.insert(QStringLiteral("was_active"), wasActive);
    queueControl(record);
    if (m_discoveryPending)
        discover();
}

void PreviewWorker::sendError(const QString &errorCode)
{
    auto record = baseRecord(QStringLiteral("error"));
    record.insert(QStringLiteral("code"), errorCode);
    queueControl(record);
}

void PreviewWorker::queueControl(QCborMap record)
{
    const QByteArray encoded = PreviewProtocol::encode(record);
    if (encoded.isEmpty())
        return;
    if (m_controlQueue.size() < 16)
        m_controlQueue.enqueue(encoded);
    flushOutput();
}

void PreviewWorker::queueFrame(QCborMap record)
{
    if (m_pendingFrame.hasFrame())
        ++m_droppedFrames;
    record.insert(QStringLiteral("dropped"), static_cast<qint64>(m_droppedFrames));
    const QByteArray encoded = PreviewProtocol::encode(record);
    if (encoded.isEmpty())
        return;
    m_pendingFrame.replace(encoded);
    flushOutput();
}

void PreviewWorker::flushOutput()
{
    for (;;)
    {
        if (m_currentOutput.isEmpty())
        {
            if (!m_controlQueue.isEmpty())
                m_currentOutput = m_controlQueue.dequeue();
            else if (m_pendingFrame.hasFrame())
                m_currentOutput = m_pendingFrame.take();
            else
            {
                m_writeNotifier->setEnabled(false);
                return;
            }
            m_outputOffset = 0;
        }

        const ssize_t written = ::write(STDOUT_FILENO, m_currentOutput.constData() + m_outputOffset,
                                        m_currentOutput.size() - m_outputOffset);
        if (written > 0)
        {
            m_outputOffset += written;
            if (m_outputOffset == m_currentOutput.size())
            {
                m_currentOutput.clear();
                m_outputOffset = 0;
            }
            continue;
        }
        if (written < 0 && errno == EAGAIN)
        {
            m_writeNotifier->setEnabled(true);
            return;
        }
        m_provider.stop();
        QCoreApplication::quit();
        return;
    }
}

QCborMap PreviewWorker::baseRecord(const QString &type)
{
    QCborMap record;
    record.insert(QStringLiteral("protocol"), PreviewProtocol::Version);
    record.insert(QStringLiteral("session"), m_sessionId);
    record.insert(QStringLiteral("sequence"), static_cast<qint64>(++m_sequence));
    record.insert(QStringLiteral("type"), type);
    return record;
}
