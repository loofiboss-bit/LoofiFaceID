// SPDX-License-Identifier: GPL-3.0-or-later

#include "previewprotocol.h"

#include <QProcess>
#include <QProcessEnvironment>
#include <QTest>

#include <utility>

class PreviewWorkerTest final : public QObject
{
    Q_OBJECT

  private Q_SLOTS:
    void rejectsSequenceReuseAndFreeArguments();
};

namespace
{
QCborMap command(const QString &session, qint64 sequence, const QString &type)
{
    return {
        {QStringLiteral("protocol"), PreviewProtocol::Version},
        {QStringLiteral("session"), session},
        {QStringLiteral("sequence"), sequence},
        {QStringLiteral("type"), type},
    };
}
} // namespace

void PreviewWorkerTest::rejectsSequenceReuseAndFreeArguments()
{
    QProcess worker;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"), QStringLiteral("offscreen"));
    worker.setProcessEnvironment(environment);
    worker.setProgram(QStringLiteral(KFACEAUTH_PREVIEW_WORKER_PATH));
    worker.start();
    QTRY_COMPARE_WITH_TIMEOUT(worker.state(), QProcess::Running, 3000);

    const QString session = QStringLiteral("worker-test-session");
    QByteArray input = PreviewProtocol::encode(command(session, 1, QStringLiteral("discover")));
    input += PreviewProtocol::encode(command(session, 1, QStringLiteral("discover")));
    QCborMap freeArgument = command(session, 2, QStringLiteral("discover"));
    freeArgument.insert(QStringLiteral("path"), QStringLiteral("/dev/video0"));
    input += PreviewProtocol::encode(freeArgument);
    for (const auto &[sequence, deadline] :
         {std::pair<qint64, qint64>{3, PreviewProtocol::monotonicMilliseconds() - 1},
          std::pair<qint64, qint64>{4, PreviewProtocol::monotonicMilliseconds() + 301000}})
    {
        QCborMap invalidStart = command(session, sequence, QStringLiteral("start"));
        invalidStart.insert(QStringLiteral("device"), QStringLiteral("invalid-token"));
        invalidStart.insert(QStringLiteral("deadline_ms"), deadline);
        input += PreviewProtocol::encode(invalidStart);
    }
    QCborMap unsolicitedBudget = command(session, 5, QStringLiteral("enrollment"));
    unsolicitedBudget.insert(QStringLiteral("deadline_ms"), PreviewProtocol::monotonicMilliseconds() + 299000);
    input += PreviewProtocol::encode(unsolicitedBudget);
    QCOMPARE(worker.write(input), input.size());

    PreviewProtocol::Parser parser;
    QVector<QCborMap> records;
    QString error;
    QTRY_VERIFY_WITH_TIMEOUT(worker.bytesAvailable() > 0, 3000);
    for (int attempt = 0; attempt < 20; ++attempt)
    {
        QVERIFY(parser.append(worker.readAllStandardOutput(), &records, &error));
        int protocolErrors = 0;
        for (const QCborMap &record : std::as_const(records))
        {
            if (record.value(QStringLiteral("type")).toString() == QLatin1String("error") &&
                record.value(QStringLiteral("code")).toString() == QLatin1String("protocol-error"))
                ++protocolErrors;
        }
        if (protocolErrors == 5)
            break;
        QTest::qWait(25);
    }

    int protocolErrors = 0;
    quint64 previousSequence = 0;
    for (const QCborMap &record : std::as_const(records))
    {
        QCOMPARE(record.value(QStringLiteral("session")).toString(), session);
        const quint64 sequence = record.value(QStringLiteral("sequence")).toInteger();
        QVERIFY(sequence > previousSequence);
        previousSequence = sequence;
        if (record.value(QStringLiteral("type")).toString() == QLatin1String("error") &&
            record.value(QStringLiteral("code")).toString() == QLatin1String("protocol-error"))
            ++protocolErrors;
    }
    QCOMPARE(protocolErrors, 5);

    worker.kill();
    QTRY_COMPARE_WITH_TIMEOUT(worker.state(), QProcess::NotRunning, 3000);
}

QTEST_GUILESS_MAIN(PreviewWorkerTest)

#include "test_previewworker.moc"
