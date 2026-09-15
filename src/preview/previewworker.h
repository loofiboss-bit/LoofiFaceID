// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "cameraprovider.h"
#include "previewprotocol.h"

#include <QByteArray>
#include <QObject>
#include <QQueue>
#include <QSharedMemory>
#include <QString>
#include <QTimer>

#include <memory>

class QSocketNotifier;

class PreviewWorker final : public QObject
{
    Q_OBJECT

  public:
    explicit PreviewWorker(QObject *parent = nullptr);
    ~PreviewWorker() override;

    bool start();

  private:
    void readCommands();
    void handleCommand(const QCborMap &command);
    void discover();
    void startPreview(const QString &token);
    void stopPreview(const QString &reason);
    void sendError(const QString &errorCode);
    void queueControl(QCborMap record);
    void queueFrame(QCborMap record);
    void flushOutput();
    [[nodiscard]] QCborMap baseRecord(const QString &type);

    CameraProvider m_provider;
    PreviewProtocol::Parser m_parser;
    std::unique_ptr<QSocketNotifier> m_readNotifier;
    std::unique_ptr<QSocketNotifier> m_writeNotifier;
    QQueue<QByteArray> m_controlQueue;
    PreviewProtocol::LatestFrameBuffer m_pendingFrame;
    QByteArray m_currentOutput;
    qsizetype m_outputOffset = 0;
    QString m_sessionId;
    quint64 m_lastCommandSequence = 0;
    quint64 m_sequence = 0;
    quint64 m_droppedFrames = 0;
    QTimer m_previewLimit;
    QSharedMemory m_sharedMemory;
};
