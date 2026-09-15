// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QCborMap>
#include <QString>
#include <QVector>

namespace PreviewProtocol
{
inline constexpr qint64 Version = 1;
inline constexpr qsizetype MaxDevices = 16;
inline constexpr qsizetype MaxLabelBytes = 128;
inline constexpr qsizetype MaxJpegBytes = 512 * 1024;
inline constexpr qsizetype MaxRawBytes = 1920 * 1080 * 3;
inline constexpr qsizetype MaxRecordBytes = MaxRawBytes + 4096;
inline constexpr int MaxWidth = 1920;
inline constexpr int MaxHeight = 1080;
inline constexpr int MaxFramesPerSecond = 30;
inline constexpr int MaxPreviewSeconds = 60;

QByteArray encode(const QCborMap &record);

class Parser
{
  public:
    bool append(QByteArrayView bytes, QVector<QCborMap> *records, QString *errorCode);
    void clear();

  private:
    QByteArray m_buffer;
};

class LatestFrameBuffer
{
  public:
    [[nodiscard]] bool hasFrame() const;
    bool replace(QByteArray frame);
    [[nodiscard]] QByteArray take();
    void clear();

  private:
    QByteArray m_frame;
};
} // namespace PreviewProtocol
