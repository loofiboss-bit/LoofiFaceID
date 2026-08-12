// SPDX-License-Identifier: BSD-2-Clause

#include <QByteArray>
#include <QCoreApplication>
#include <QFile>
#include <QProcessEnvironment>
#include <QtEndian>

#include <chrono>
#include <cstdio>
#include <thread>

namespace
{
bool writeAll(QByteArray bytes)
{
    QFile output;
    if (!output.open(stdout, QIODevice::WriteOnly))
        return false;
    while (!bytes.isEmpty())
    {
        const qint64 written = output.write(bytes);
        if (written <= 0)
            return false;
        bytes.remove(0, written);
    }
    return output.flush();
}

QByteArray framedResponse(QByteArrayView request, const QString &mode)
{
    if (mode == QLatin1String("malformed"))
    {
        QByteArray framed(4, 0);
        qToBigEndian(quint32(3), framed.data());
        framed.append(QByteArray(3, 0));
        return framed;
    }

    if (request.size() < 16)
        return {};
    const quint8 operation = static_cast<quint8>(request.at(6));
    quint64 generation = qFromBigEndian<quint64>(reinterpret_cast<const uchar *>(request.data() + 8));
    quint8 kind = 0x83;
    quint8 code = 0;
    QByteArray body;

    if (mode == QLatin1String("session") || mode == QLatin1String("session-hang-capture") ||
        mode == QLatin1String("session-lifecycle") || mode == QLatin1String("session-fail-commit"))
    {
        if (operation == 1)
        {
            kind = 0x81;
            if (mode == QLatin1String("session-lifecycle") || mode == QLatin1String("session-fail-commit"))
            {
                code = 1;
                body.append(char(5));
            }
            else
            {
                body.append(char(0));
            }
        }
        else if (operation == 2)
        {
            kind = 0x82;
            body = QByteArray(128 * 4, char(0x2a));
        }
        else if (operation == 3 && mode == QLatin1String("session-fail-commit"))
        {
            kind = 0xff;
            code = 20;
        }
        else if (operation == 5)
        {
            kind = 0x84;
            code = 1;
        }
    }
    if (mode == QLatin1String("stale"))
        ++generation;

    QByteArray payload(12, 0);
    qToBigEndian(quint16(1), payload.data());
    payload[2] = static_cast<char>(kind);
    payload[3] = static_cast<char>(code);
    qToBigEndian(generation, payload.data() + 4);
    payload.append(body);
    QByteArray framed(4, 0);
    qToBigEndian(static_cast<quint32>(payload.size()), framed.data());
    framed.append(payload);
    return framed;
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);
    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly))
        return 2;
    QByteArray request = input.readAll();

    const QString mode = QProcessEnvironment::systemEnvironment().value(QStringLiteral("KFACEAUTH_TEST_MODE"));
    if (mode == QLatin1String("crash"))
    {
        request.fill(0);
        return 7;
    }
    if (mode == QLatin1String("hang"))
        std::this_thread::sleep_for(std::chrono::seconds(30));
    if (mode == QLatin1String("delayed"))
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    if (mode == QLatin1String("session-hang-capture") && request.size() >= 7 && static_cast<quint8>(request.at(6)) == 2)
        std::this_thread::sleep_for(std::chrono::seconds(30));

    QByteArray response = framedResponse(request, mode);
    request.fill(0);
    request.clear();
    const bool written = !response.isEmpty() && writeAll(response);
    response.fill(0);
    return written ? 0 : 3;
}
