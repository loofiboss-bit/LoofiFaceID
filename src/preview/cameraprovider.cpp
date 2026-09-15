// SPDX-License-Identifier: GPL-3.0-or-later

#include "cameraprovider.h"

#include "previewprotocol.h"

#include <QBuffer>
#include <QCamera>
#include <QCoreApplication>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QPermissions>
#include <QRandomGenerator>
#include <QVideoFrame>
#include <QVideoSink>

#include <fcntl.h>
#include <libudev.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>

namespace
{
QString userText(const char *text)
{
    return QCoreApplication::translate("CameraProvider", text);
}
} // namespace

CameraProvider::CameraProvider(QObject *parent) : QObject(parent)
{
    connect(new QMediaDevices(this), &QMediaDevices::videoInputsChanged, this, &CameraProvider::deviceListChanged);
}

CameraProvider::~CameraProvider()
{
    stop();
}

QVector<CameraDescriptor> CameraProvider::discover()
{
    stop();
    m_devices.clear();
    const QList<QCameraDevice> inputs = QMediaDevices::videoInputs();
    const qsizetype count = std::min(inputs.size(), PreviewProtocol::MaxDevices);
    m_devices.reserve(count);
    for (qsizetype index = 0; index < count; ++index)
    {
        const QCameraDevice &device = inputs.at(index);
        const QString node = deviceNode(device);
        CameraDescriptor descriptor;
        descriptor.token = QString::number(QRandomGenerator::global()->generate64(), 16);
        descriptor.label = sanitizedLabel(device.description());
        descriptor.spectrum = spectrumForNode(node);
        descriptor.device = device;
        descriptor.deviceNode = node;
        m_devices.push_back(descriptor);
    }
    return m_devices;
}

bool CameraProvider::start(const QString &token, QString *errorCode)
{
    if (!errorCode)
        return false;
    stop();
    const auto iterator = std::find_if(m_devices.cbegin(), m_devices.cend(),
                                       [&token](const auto &device) { return device.token == token; });
    if (iterator == m_devices.cend())
    {
        *errorCode = QStringLiteral("no-camera");
        return false;
    }

    const QCameraPermission permission;
    if (QCoreApplication::instance()->checkPermission(permission) == Qt::PermissionStatus::Denied)
    {
        *errorCode = QStringLiteral("permission-denied");
        return false;
    }

    const QString preflightError = preflightNode(iterator->deviceNode);
    if (!preflightError.isEmpty())
    {
        *errorCode = preflightError;
        return false;
    }

    const QCameraFormat format = selectFormat(iterator->device.videoFormats());
    if (format.isNull())
    {
        *errorCode = QStringLiteral("format-unavailable");
        return false;
    }

    m_spectrum = iterator->spectrum;
    m_captureSession = std::make_unique<QMediaCaptureSession>();
    m_videoSink = std::make_unique<QVideoSink>();
    m_camera = std::make_unique<QCamera>(iterator->device);
    m_camera->setCameraFormat(format);
    m_captureSession->setCamera(m_camera.get());
    m_captureSession->setVideoSink(m_videoSink.get());
    connect(m_videoSink.get(), &QVideoSink::videoFrameChanged, this, &CameraProvider::handleFrame);
    connect(m_camera.get(), &QCamera::activeChanged, this,
            [this](bool active)
            {
                if (active)
                    Q_EMIT started();
            });
    connect(m_camera.get(), &QCamera::errorOccurred, this,
            [this](QCamera::Error error)
            {
                if (error != QCamera::NoError)
                    Q_EMIT failed(QStringLiteral("camera-unavailable"));
            });
    m_frameThrottle.invalidate();
    m_camera->start();
    return true;
}

void CameraProvider::stop()
{
    if (m_camera)
        m_camera->stop();
    m_videoSink.reset();
    m_captureSession.reset();
    m_camera.reset();
    m_spectrum.clear();
    m_frameThrottle.invalidate();
}

bool CameraProvider::active() const
{
    return m_camera && m_camera->isActive();
}

QCameraFormat CameraProvider::selectFormat(const QList<QCameraFormat> &formats)
{
    if (formats.isEmpty())
        return {};
    QVector<CameraFormatCandidate> candidates;
    candidates.reserve(formats.size());
    for (const QCameraFormat &format : formats)
        candidates.push_back({format.resolution(), format.maxFrameRate()});
    const int index = selectFormatIndex(candidates);
    return index < 0 ? QCameraFormat{} : formats.at(index);
}

int CameraProvider::selectFormatIndex(const QVector<CameraFormatCandidate> &formats)
{
    if (formats.isEmpty())
        return -1;
    auto score = [](const CameraFormatCandidate &format)
    {
        const QSize size = format.resolution;
        const bool bounded = size.width() <= PreviewProtocol::MaxWidth && size.height() <= PreviewProtocol::MaxHeight;
        const bool usableRate = format.maxFrameRate >= 8.0;
        const qint64 area = static_cast<qint64>(size.width()) * size.height();
        return std::tuple{bounded, usableRate, bounded ? area : -area};
    };
    const auto selected =
        std::max_element(formats.cbegin(), formats.cend(),
                         [&score](const auto &left, const auto &right) { return score(left) < score(right); });
    return static_cast<int>(std::distance(formats.cbegin(), selected));
}

QByteArray CameraProvider::encodeFrame(const QImage &source)
{
    if (source.isNull())
        return {};
    QImage image = source;
    if (image.width() > PreviewProtocol::MaxWidth || image.height() > PreviewProtocol::MaxHeight)
        image = image.scaled(PreviewProtocol::MaxWidth, PreviewProtocol::MaxHeight, Qt::KeepAspectRatio,
                             Qt::SmoothTransformation);
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    if (buffer.open(QIODevice::WriteOnly) && image.save(&buffer, "JPEG", 75) &&
        jpeg.size() <= PreviewProtocol::MaxJpegBytes)
        return jpeg;
    return {};
}

QString CameraProvider::sanitizedLabel(const QString &label)
{
    QString sanitized;
    sanitized.reserve(std::min(label.size(), PreviewProtocol::MaxLabelBytes));
    for (const QChar character : label)
    {
        if (character.category() != QChar::Other_Control)
            sanitized.append(character);
        if (sanitized.toUtf8().size() >= PreviewProtocol::MaxLabelBytes)
            break;
    }
    return sanitized.trimmed().isEmpty() ? userText("Camera") : sanitized.trimmed();
}

QString CameraProvider::spectrumForNode(const QString &node)
{
    if (node.isEmpty())
        return QStringLiteral("unknown");
    struct stat metadata{};
    if (::stat(node.toLocal8Bit().constData(), &metadata) != 0)
        return QStringLiteral("unknown");
    udev *context = udev_new();
    if (!context)
        return QStringLiteral("unknown");
    udev_device *device = udev_device_new_from_devnum(context, 'c', metadata.st_rdev);
    if (!device)
    {
        udev_unref(context);
        return QStringLiteral("unknown");
    }
    const char *infrared = udev_device_get_property_value(device, "ID_INFRARED_CAMERA");
    const char *capabilities = udev_device_get_property_value(device, "ID_V4L_CAPABILITIES");
    const QString spectrum = classifyProperties(infrared ? QByteArrayView(infrared) : QByteArrayView{},
                                                capabilities ? QByteArrayView(capabilities) : QByteArrayView{});
    udev_device_unref(device);
    udev_unref(context);
    return spectrum;
}

QString CameraProvider::classifyProperties(QByteArrayView infraredProperty, QByteArrayView capabilitiesProperty)
{
    if (infraredProperty == "1")
        return QStringLiteral("ir");
    if (capabilitiesProperty.contains(":capture:"))
        return QStringLiteral("rgb");
    return QStringLiteral("unknown");
}

QString CameraProvider::deviceNode(const QCameraDevice &device)
{
    const QByteArray id = device.id();
    if (!id.startsWith("/dev/video"))
        return {};
    const QString node = QString::fromLocal8Bit(id);
    for (const QChar character : node)
    {
        if (!character.isLetterOrNumber() && character != QLatin1Char('/'))
            return {};
    }
    return node;
}

QString CameraProvider::preflightNode(const QString &node)
{
    if (node.isEmpty())
        return {};
    const int descriptor = ::open(node.toLocal8Bit().constData(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (descriptor >= 0)
    {
        ::close(descriptor);
        return {};
    }
    if (errno == EBUSY)
        return QStringLiteral("camera-busy");
    if (errno == EACCES || errno == EPERM)
        return QStringLiteral("permission-denied");
    return QStringLiteral("camera-unavailable");
}

void CameraProvider::handleFrame(const QVideoFrame &frame)
{
    if (!frame.isValid())
        return;
    const qint64 minimumInterval = 1000 / PreviewProtocol::MaxFramesPerSecond;
    if (m_frameThrottle.isValid() && m_frameThrottle.elapsed() < minimumInterval)
        return;
    m_frameThrottle.restart();
    const QImage image = frame.toImage();
    const QImage bounded =
        image.width() > PreviewProtocol::MaxWidth || image.height() > PreviewProtocol::MaxHeight
            ? image.scaled(PreviewProtocol::MaxWidth, PreviewProtocol::MaxHeight, Qt::KeepAspectRatio)
            : image;
    const QImage rgb = bounded.convertToFormat(QImage::Format_RGB888);
    const QByteArray rawRgb(reinterpret_cast<const char *>(rgb.constBits()), rgb.sizeInBytes());
    Q_EMIT rawFrameReady(rawRgb, bounded.width(), bounded.height(), m_spectrum);
    const QByteArray jpeg = encodeFrame(bounded);
    if (!jpeg.isEmpty())
        Q_EMIT frameReady(jpeg, bounded.width(), bounded.height(), m_spectrum);
}
