// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "previewprotocol.h"

#include <QAbstractListModel>
#include <QImage>
#include <QProcess>
#include <QString>
#include <QTimer>
#include <QVector>

class CameraPreviewSession final : public QAbstractListModel
{
    Q_OBJECT

    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool previewActive READ previewActive NOTIFY stateChanged)
    Q_PROPERTY(bool canStartPreview READ canStartPreview NOTIFY stateChanged)
    Q_PROPERTY(bool canStopPreview READ canStopPreview NOTIFY stateChanged)
    Q_PROPERTY(bool canRefresh READ canRefresh NOTIFY stateChanged)
    Q_PROPERTY(bool hasUsableCamera READ hasUsableCamera NOTIFY stateChanged)
    Q_PROPERTY(int deviceCount READ deviceCount NOTIFY devicesChanged)
    Q_PROPERTY(int selectedDeviceIndex READ selectedDeviceIndex WRITE setSelectedDeviceIndex NOTIFY selectionChanged)
    Q_PROPERTY(bool frameAvailable READ frameAvailable NOTIFY frameChanged)
    Q_PROPERTY(QString spectrum READ spectrum NOTIFY frameChanged)
    Q_PROPERTY(int remainingSeconds READ remainingSeconds NOTIFY stateChanged)
    Q_PROPERTY(quint64 droppedFrames READ droppedFrames NOTIFY stateChanged)
    Q_PROPERTY(quint64 frameRevision READ frameRevision NOTIFY frameChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QString errorCode READ errorCode NOTIFY stateChanged)

  public:
    enum class State
    {
        Idle,
        Discovering,
        Ready,
        Starting,
        Streaming,
        Stopping,
        Failed,
    };
    Q_ENUM(State)

    enum Role
    {
        LabelRole = Qt::UserRole + 1,
        SpectrumRole,
    };

    explicit CameraPreviewSession(QObject *parent = nullptr);
    CameraPreviewSession(QString workerPath, QObject *parent);
    ~CameraPreviewSession() override;

    [[nodiscard]] int rowCount(const QModelIndex &parent = {}) const override;
    [[nodiscard]] QVariant data(const QModelIndex &index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    [[nodiscard]] State state() const;
    [[nodiscard]] bool busy() const;
    [[nodiscard]] bool previewActive() const;
    [[nodiscard]] bool canStartPreview() const;
    [[nodiscard]] bool canStopPreview() const;
    [[nodiscard]] bool canRefresh() const;
    [[nodiscard]] bool hasUsableCamera() const;
    [[nodiscard]] int deviceCount() const;
    [[nodiscard]] int selectedDeviceIndex() const;
    void setSelectedDeviceIndex(int index);
    [[nodiscard]] bool frameAvailable() const;
    [[nodiscard]] QString spectrum() const;
    [[nodiscard]] int remainingSeconds() const;
    [[nodiscard]] quint64 droppedFrames() const;
    [[nodiscard]] quint64 frameRevision() const;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QString errorCode() const;
    [[nodiscard]] QImage frame() const;
    [[nodiscard]] bool copyCurrentFrame(QImage *destination) const;
    [[nodiscard]] int deviceCountForSpectrum(const QString &spectrum) const;

    Q_INVOKABLE void refreshDevices();
    Q_INVOKABLE void startPreview();
    Q_INVOKABLE void stopPreview();
    void clearFrame();

  Q_SIGNALS:
    void stateChanged();
    void devicesChanged();
    void selectionChanged();
    void frameChanged();

  private:
    struct Device
    {
        QString token;
        QString label;
        QString spectrum;
    };

    void startWorker();
    void sendCommand(const QString &type, const QString &deviceToken = {});
    void processRecords();
    bool handleRecord(const QCborMap &record);
    bool handleDevices(const QCborMap &record);
    bool handleFrame(const QCborMap &record);
    void setState(State state, const QString &statusText);
    void fail(const QString &errorCode);
    void terminateWorker();
    void resetWorker(bool expected);
    [[nodiscard]] QString textForError(const QString &errorCode) const;

    QString m_workerPath;
    QProcess *m_process = nullptr;
    PreviewProtocol::Parser m_parser;
    QVector<Device> m_devices;
    State m_state = State::Idle;
    int m_selectedDeviceIndex = -1;
    QImage m_frame;
    QString m_spectrum;
    QString m_statusText;
    QString m_errorCode;
    QString m_sessionId;
    quint64 m_nextCommandSequence = 0;
    quint64 m_lastSequence = 0;
    quint64 m_droppedFrames = 0;
    quint64 m_frameRevision = 0;
    int m_remainingSeconds = 0;
    qsizetype m_stderrBytes = 0;
    bool m_expectedExit = false;
    QTimer m_startupTimer;
    QTimer m_stallTimer;
    QTimer m_stopTimer;
    QTimer m_countdownTimer;
};
