// SPDX-License-Identifier: GPL-3.0-or-later

#include "previewprotocol.h"

#include <QBuffer>
#include <QCborArray>
#include <QCoreApplication>
#include <QImage>
#include <QSocketNotifier>
#include <QTimer>

#include <unistd.h>

#include <initializer_list>
#include <utility>

class FakeWorker final : public QObject
{
  public:
    explicit FakeWorker(QObject *parent = nullptr) : QObject(parent)
    {
        connect(&m_notifier, &QSocketNotifier::activated, this, [this]() { readCommands(); });
    }

  private:
    void readCommands()
    {
        char bytes[4096];
        const ssize_t count = ::read(STDIN_FILENO, bytes, sizeof(bytes));
        if (count <= 0)
        {
            QCoreApplication::quit();
            return;
        }
        QVector<QCborMap> commands;
        QString error;
        if (!m_parser.append(QByteArrayView(bytes, count), &commands, &error))
            return;
        for (const QCborMap &command : commands)
        {
            m_session = command.value(QStringLiteral("session")).toString();
            const QString type = command.value(QStringLiteral("type")).toString();
            if (type == QLatin1String("discover"))
            {
                QCborArray devices;
                devices.append(QCborMap{{QStringLiteral("token"), QStringLiteral("rgb-token")},
                                        {QStringLiteral("label"), QStringLiteral("RGB Test Camera")},
                                        {QStringLiteral("spectrum"), QStringLiteral("rgb")}});
                devices.append(QCborMap{{QStringLiteral("token"), QStringLiteral("ir-token")},
                                        {QStringLiteral("label"), QStringLiteral("IR Test Camera")},
                                        {QStringLiteral("spectrum"), QStringLiteral("ir")}});
                for (const auto &[token, label] : std::initializer_list<std::pair<QString, QString>>{
                         {QStringLiteral("busy-token"), QStringLiteral("Busy Test Camera")},
                         {QStringLiteral("crash-token"), QStringLiteral("Crash Test Camera")},
                         {QStringLiteral("no-start-token"), QStringLiteral("Startup Timeout Camera")},
                         {QStringLiteral("stall-token"), QStringLiteral("Stalled Test Camera")},
                         {QStringLiteral("no-stop-token"), QStringLiteral("Stop Timeout Camera")},
                         {QStringLiteral("timeout-token"), QStringLiteral("Time Limit Camera")},
                         {QStringLiteral("unplug-token"), QStringLiteral("Hot Unplug Camera")},
                         {QStringLiteral("bad-sequence-token"), QStringLiteral("Protocol Test Camera")},
                     })
                {
                    devices.append(QCborMap{{QStringLiteral("token"), token},
                                            {QStringLiteral("label"), label},
                                            {QStringLiteral("spectrum"), QStringLiteral("unknown")}});
                }
                QCborMap response = base(QStringLiteral("devices"));
                response.insert(QStringLiteral("devices"), devices);
                send(response);
            }
            else if (type == QLatin1String("enrollment"))
            {
                QCborMap response = base(QStringLiteral("budget"));
                response.insert(QStringLiteral("deadline_ms"), command.value(QStringLiteral("deadline_ms")));
                send(response);
            }
            else if (type == QLatin1String("start"))
            {
                m_activeToken = command.value(QStringLiteral("device")).toString();
                if (m_activeToken == QLatin1String("busy-token"))
                {
                    QCborMap errorRecord = base(QStringLiteral("error"));
                    errorRecord.insert(QStringLiteral("code"), QStringLiteral("camera-busy"));
                    send(errorRecord);
                    continue;
                }
                if (m_activeToken == QLatin1String("crash-token"))
                    ::_exit(12);
                if (m_activeToken == QLatin1String("no-start-token"))
                    continue;

                QCborMap started = base(QStringLiteral("started"));
                started.insert(QStringLiteral("seconds"), PreviewProtocol::MaxPreviewSeconds);
                send(started);
                if (m_activeToken == QLatin1String("stall-token"))
                    continue;

                QImage image(16, 12, QImage::Format_RGB32);
                image.fill(QColor(QStringLiteral("#506070")));
                QByteArray jpeg;
                QBuffer buffer(&jpeg);
                buffer.open(QIODevice::WriteOnly);
                image.save(&buffer, "JPEG", 80);
                QCborMap frame = base(QStringLiteral("frame"));
                frame.insert(QStringLiteral("jpeg"), jpeg);
                frame.insert(QStringLiteral("width"), image.width());
                frame.insert(QStringLiteral("height"), image.height());
                frame.insert(QStringLiteral("spectrum"),
                             m_activeToken == QLatin1String("ir-token")
                                 ? QStringLiteral("ir")
                                 : (m_activeToken == QLatin1String("rgb-token") ? QStringLiteral("rgb")
                                                                                : QStringLiteral("unknown")));
                frame.insert(QStringLiteral("dropped"), 0);
                if (m_activeToken == QLatin1String("bad-sequence-token"))
                    frame.insert(QStringLiteral("sequence"), static_cast<qint64>(m_sequence - 1));
                send(frame);
                if (m_activeToken == QLatin1String("timeout-token"))
                {
                    QTimer::singleShot(100, this,
                                       [this]()
                                       {
                                           QCborMap stopped = base(QStringLiteral("stopped"));
                                           stopped.insert(QStringLiteral("reason"), QStringLiteral("time-limit"));
                                           stopped.insert(QStringLiteral("was_active"), true);
                                           send(stopped);
                                       });
                }
                if (m_activeToken == QLatin1String("unplug-token"))
                {
                    QTimer::singleShot(100, this,
                                       [this]()
                                       {
                                           QCborMap errorRecord = base(QStringLiteral("error"));
                                           errorRecord.insert(QStringLiteral("code"), QStringLiteral("no-camera"));
                                           send(errorRecord);
                                       });
                }
            }
            else if (type == QLatin1String("stop"))
            {
                if (m_activeToken == QLatin1String("no-stop-token"))
                    continue;
                QCborMap stopped = base(QStringLiteral("stopped"));
                stopped.insert(QStringLiteral("reason"), QStringLiteral("requested"));
                stopped.insert(QStringLiteral("was_active"), true);
                send(stopped);
            }
        }
    }

    QCborMap base(const QString &type)
    {
        return {
            {QStringLiteral("protocol"), PreviewProtocol::Version},
            {QStringLiteral("session"), m_session},
            {QStringLiteral("sequence"), static_cast<qint64>(++m_sequence)},
            {QStringLiteral("type"), type},
        };
    }

    static void send(const QCborMap &record)
    {
        const QByteArray bytes = PreviewProtocol::encode(record);
        qsizetype offset = 0;
        while (offset < bytes.size())
        {
            const ssize_t written = ::write(STDOUT_FILENO, bytes.constData() + offset, bytes.size() - offset);
            if (written <= 0)
                return;
            offset += written;
        }
    }

    QSocketNotifier m_notifier{STDIN_FILENO, QSocketNotifier::Read};
    PreviewProtocol::Parser m_parser;
    QString m_session;
    QString m_activeToken;
    quint64 m_sequence = 0;
};

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    FakeWorker worker;
    return application.exec();
}
