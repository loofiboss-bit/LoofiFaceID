// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QCameraDevice>
#include <QCameraFormat>
#include <QElapsedTimer>
#include <QImage>
#include <QObject>
#include <QString>
#include <QVector>

#include <memory>

class QCamera;
class QMediaCaptureSession;
class QVideoFrame;
class QVideoSink;

struct CameraDescriptor
{
    QString token;
    QString label;
    QString spectrum;
    QCameraDevice device;
    QString deviceNode;
};

struct CameraFormatCandidate
{
    QSize resolution;
    qreal maxFrameRate = 0.0;
};

class CameraProvider final : public QObject
{
    Q_OBJECT

  public:
    explicit CameraProvider(QObject *parent = nullptr);
    ~CameraProvider() override;

    [[nodiscard]] QVector<CameraDescriptor> discover();
    bool start(const QString &token, QString *errorCode);
    void stop();
    [[nodiscard]] bool active() const;

    [[nodiscard]] static QCameraFormat selectFormat(const QList<QCameraFormat> &formats);
    [[nodiscard]] static int selectFormatIndex(const QVector<CameraFormatCandidate> &formats);
    [[nodiscard]] static QByteArray encodeFrame(const QImage &image);
    [[nodiscard]] static QString classifyProperties(QByteArrayView infraredProperty,
                                                    QByteArrayView capabilitiesProperty);

  Q_SIGNALS:
    void started();
    void frameReady(const QByteArray &jpeg, int width, int height, const QString &spectrum);
    void rawFrameReady(const QByteArray &rgb, int width, int height, const QString &spectrum);
    void failed(const QString &errorCode);
    void deviceListChanged();

  private:
    [[nodiscard]] static QString sanitizedLabel(const QString &label);
    [[nodiscard]] static QString spectrumForNode(const QString &node);
    [[nodiscard]] static QString deviceNode(const QCameraDevice &device);
    [[nodiscard]] static QString preflightNode(const QString &node);
    void handleFrame(const QVideoFrame &frame);

    QVector<CameraDescriptor> m_devices;
    std::unique_ptr<QCamera> m_camera;
    std::unique_ptr<QMediaCaptureSession> m_captureSession;
    std::unique_ptr<QVideoSink> m_videoSink;
    QElapsedTimer m_frameThrottle;
    QString m_spectrum;
};
