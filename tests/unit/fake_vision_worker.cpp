// SPDX-License-Identifier: GPL-3.0-or-later

#include <QByteArray>
#include <QCoreApplication>
#include <QProcessEnvironment>
#include <QSocketNotifier>
#include <QStringList>
#include <QTimer>
#include <QtEndian>

#include <unistd.h>

#include <algorithm>

namespace
{
constexpr qsizetype RequestHeaderBytes = 24;
constexpr qsizetype MaxPayloadBytes = 1920 * 1080 * 4 + RequestHeaderBytes;

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

void writeAll(QByteArrayView bytes)
{
    qsizetype offset = 0;
    while (offset < bytes.size())
    {
        const ssize_t written = ::write(STDOUT_FILENO, bytes.data() + offset, bytes.size() - offset);
        if (written <= 0)
            return;
        offset += written;
    }
}

QByteArray framed(QByteArray payload)
{
    QByteArray response;
    appendU32(&response, static_cast<quint32>(payload.size()));
    response.append(payload);
    return response;
}
} // namespace

class FakeVisionWorker final : public QObject
{
  public:
    explicit FakeVisionWorker(QObject *parent = nullptr)
        : QObject(parent),
          m_mode(QProcessEnvironment::systemEnvironment().value(QStringLiteral("KFACEAUTH_FAKE_VISION_MODE"))),
          m_sessionMode(QCoreApplication::arguments().contains(QStringLiteral("--session")))
    {
        connect(&m_notifier, &QSocketNotifier::activated, this, [this]() { readRequest(); });
    }

  private:
    void sendError(quint8 code, quint64 generation, bool continueSession)
    {
        QByteArray response;
        appendU16(&response, 2);
        response.append(static_cast<char>(0xff));
        response.append(static_cast<char>(code));
        appendU64(&response, generation);
        writeAll(framed(response));
        m_request.fill(0);
        m_request.clear();
        if (continueSession && m_sessionMode)
            m_notifier.setEnabled(true);
        else
            QCoreApplication::quit();
    }

    void readRequest()
    {
        char bytes[16384];
        const ssize_t count = ::read(STDIN_FILENO, bytes, sizeof(bytes));
        if (count < 0)
            return;
        if (count == 0)
        {
            handleRequest();
            return;
        }
        if (m_request.size() + count > MaxPayloadBytes + 4)
        {
            QCoreApplication::exit(2);
            return;
        }
        m_request.append(bytes, count);
        if (m_request.size() >= 4)
        {
            const quint32 payloadSize = readU32(QByteArrayView(m_request), 0);
            if (payloadSize > MaxPayloadBytes)
            {
                QCoreApplication::exit(2);
                return;
            }
            if (m_request.size() == static_cast<qsizetype>(payloadSize) + 4)
                handleRequest();
        }
    }

    void handleRequest()
    {
        m_notifier.setEnabled(false);
        if (m_request.size() < RequestHeaderBytes + 4)
        {
            QCoreApplication::exit(2);
            return;
        }
        const QByteArrayView payload(m_request.constData() + 4, m_request.size() - 4);
        const quint32 declaredSize = readU32(QByteArrayView(m_request), 0);
        const quint16 width = readU16(payload, 16);
        const quint16 height = readU16(payload, 18);
        const quint32 stride = readU32(payload, 20);
        if (declaredSize != payload.size() || readU16(payload, 0) != 2 || static_cast<quint8>(payload.at(2)) != 1 ||
            static_cast<quint8>(payload.at(3)) != 1 || width == 0 || width > 1920 || height == 0 || height > 1080 ||
            stride < static_cast<quint32>(width) * 3 || stride > static_cast<quint32>(width) * 4 ||
            payload.size() != RequestHeaderBytes + static_cast<qsizetype>(stride) * height)
        {
            QCoreApplication::exit(2);
            return;
        }

        const quint64 generation = readU64(payload, 4);
        QString mode = m_mode;
        if (mode.isEmpty())
        {
            const quint8 marker = static_cast<quint8>(payload.at(RequestHeaderBytes));
            if (marker <= 7)
            {
                static const QStringList modes = {
                    QStringLiteral("zero"),     QStringLiteral("one"),
                    QStringLiteral("multiple"), QStringLiteral("crash"),
                    QStringLiteral("timeout"),  QStringLiteral("malformed"),
                    QStringLiteral("stale"),    QStringLiteral("shutdown-timeout"),
                };
                mode = modes.at(marker);
            }
            else
                mode = QStringLiteral("one");
        }

        if (mode == QLatin1String("edge-once"))
        {
            if (m_requestCount++ == 0)
            {
                sendError(13, generation, true);
                return;
            }
            mode = QStringLiteral("one");
        }
        else if (mode == QLatin1String("edge-always"))
        {
            ++m_requestCount;
            sendError(13, generation, m_sessionMode);
            return;
        }

        if (mode == QLatin1String("crash"))
            ::_exit(17);
        if (mode == QLatin1String("timeout"))
            return;
        if (mode == QLatin1String("malformed"))
        {
            writeAll(framed(QByteArray(12, '\0')));
            QCoreApplication::quit();
            return;
        }
        if (mode == QLatin1String("oversized"))
        {
            QByteArray prefix;
            appendU32(&prefix, 81);
            writeAll(prefix);
            return;
        }
        if (mode == QLatin1String("model-unavailable") || mode == QLatin1String("runtime-output") ||
            mode == QLatin1String("runtime-failure") || mode == QLatin1String("legacy-internal"))
        {
            const quint8 code =
                mode == QLatin1String("model-unavailable")
                    ? 11
                    : (mode == QLatin1String("runtime-output") ? 14
                                                               : (mode == QLatin1String("runtime-failure") ? 15 : 12));
            sendError(code, generation, false);
            return;
        }

        const quint8 faceCount = mode == QLatin1String("zero") ? 0 : (mode == QLatin1String("multiple") ? 2 : 1);
        QByteArray response;
        appendU16(&response, 2);
        response.append(static_cast<char>(0x81));
        response.append(static_cast<char>(faceCount));
        appendU64(&response, mode == QLatin1String("stale") ? generation + 1 : generation);
        response.append(static_cast<char>(128));
        response.append(static_cast<char>(120));
        response.append(static_cast<char>(110));
        response.append(mode == QLatin1String("unknown-flags")
                            ? static_cast<char>(0x80)
                            : (mode == QLatin1String("contradictory-flags") ? static_cast<char>(0x03) : '\0'));

        for (quint8 index = 0; index < faceCount; ++index)
        {
            const quint16 rectWidth = std::max<quint16>(1, width / 2);
            const quint16 rectHeight = std::max<quint16>(1, height / 2);
            const quint16 x = index == 0 ? (width - rectWidth) / 2 : 0;
            const quint16 y = index == 0 ? (height - rectHeight) / 2 : 0;
            appendU16(&response, x);
            appendU16(&response, y);
            appendU16(&response, rectWidth);
            appendU16(&response, rectHeight);
            const bool naturalPose = mode == QLatin1String("natural");
            const quint16 firstEyeX = naturalPose ? x + 1 : x + rectWidth / 4;
            const quint16 secondEyeX = naturalPose ? x + rectWidth - 1 : x + (rectWidth * 3) / 4;
            const quint16 noseX =
                mode == QLatin1String("left")
                    ? x + rectWidth / 4
                    : (mode == QLatin1String("right") ? x + (rectWidth * 3) / 4
                                                      : (naturalPose ? x + rectWidth / 2 + 1 : x + rectWidth / 2));
            const quint16 noseY = mode == QLatin1String("tilt") ? y + (rectHeight * 2) / 3 : y + rectHeight / 2;
            appendU16(&response, mode == QLatin1String("landmark-outside")
                                     ? width
                                     : (mode == QLatin1String("landmark-outside-rect") ? x + rectWidth : firstEyeX));
            appendU16(&response, y + rectHeight / 3);
            appendU16(&response, secondEyeX);
            appendU16(&response, y + rectHeight / 3);
            appendU16(&response, noseX);
            appendU16(&response, noseY);
            appendU16(&response, x + rectWidth / 4);
            appendU16(&response, y + (rectHeight * 2) / 3);
            appendU16(&response, x + (rectWidth * 3) / 4);
            appendU16(&response, y + (rectHeight * 2) / 3);
        }
        writeAll(framed(response));
        m_request.fill(0);
        m_request.clear();
        if (m_sessionMode)
        {
            m_notifier.setEnabled(true);
            return;
        }
        if (mode != QLatin1String("shutdown-timeout"))
            QCoreApplication::quit();
    }

    QSocketNotifier m_notifier{STDIN_FILENO, QSocketNotifier::Read};
    QByteArray m_request;
    QString m_mode;
    bool m_sessionMode = false;
    int m_requestCount = 0;
};

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    FakeVisionWorker worker;
    return application.exec();
}
