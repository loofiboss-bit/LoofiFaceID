// SPDX-License-Identifier: GPL-3.0-or-later

#include "cameraprovider.h"
#include "previewprotocol.h"

#include <QImage>
#include <QTest>

class CameraProviderTest final : public QObject
{
    Q_OBJECT

  private Q_SLOTS:
    void classifiesOnlyReviewedUdevProperties();
    void selectsBestBoundedFormat();
    void scalesAndBoundsJpeg();
};

void CameraProviderTest::classifiesOnlyReviewedUdevProperties()
{
    QCOMPARE(CameraProvider::classifyProperties("1", ":capture:"), QStringLiteral("ir"));
    QCOMPARE(CameraProvider::classifyProperties({}, ":capture:"), QStringLiteral("rgb"));
    QCOMPARE(CameraProvider::classifyProperties({}, {}), QStringLiteral("unknown"));
    QCOMPARE(CameraProvider::classifyProperties({}, "Infrared Camera"), QStringLiteral("unknown"));
}

void CameraProviderTest::selectsBestBoundedFormat()
{
    const QVector<CameraFormatCandidate> formats = {
        {QSize(1920, 1080), 30.0},
        {QSize(320, 240), 30.0},
        {QSize(640, 480), 8.0},
        {QSize(640, 480), 5.0},
    };
    QCOMPARE(CameraProvider::selectFormatIndex(formats), 0);
    QCOMPARE(CameraProvider::selectFormatIndex({}), -1);
}

void CameraProviderTest::scalesAndBoundsJpeg()
{
    QImage image(1920, 1080, QImage::Format_RGB32);
    for (int y = 0; y < image.height(); ++y)
    {
        auto *line = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x)
            line[x] = qRgb((x * 17 + y * 3) % 256, (x * 5 + y * 11) % 256, (x + y * 7) % 256);
    }

    const QByteArray jpeg = CameraProvider::encodeFrame(image);
    QVERIFY(!jpeg.isEmpty());
    QVERIFY(jpeg.size() <= PreviewProtocol::MaxJpegBytes);
    const QImage decoded = QImage::fromData(jpeg, "JPEG");
    QVERIFY(!decoded.isNull());
    QVERIFY(decoded.width() <= PreviewProtocol::MaxWidth);
    QVERIFY(decoded.height() <= PreviewProtocol::MaxHeight);
}

QTEST_GUILESS_MAIN(CameraProviderTest)

#include "test_cameraprovider.moc"
