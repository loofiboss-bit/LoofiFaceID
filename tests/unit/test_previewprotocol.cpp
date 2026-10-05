// SPDX-License-Identifier: GPL-3.0-or-later

#include "previewprotocol.h"

#include <QTest>
#include <QtEndian>
#include <limits>

class PreviewProtocolTest final : public QObject
{
    Q_OBJECT

  private Q_SLOTS:
    void fragmentedRecord();
    void deadlinesAreMonotonicAndBounded();
    void rejectsOversizedRecord();
    void rejectsInvalidCbor();
    void latestFrameWinsBackpressure();
};

void PreviewProtocolTest::deadlinesAreMonotonicAndBounded()
{
    const qint64 now = PreviewProtocol::monotonicMilliseconds();
    QVERIFY(!PreviewProtocol::validDeadline(std::numeric_limits<qint64>::min(), PreviewProtocol::MaxEnrollmentSeconds));
    QVERIFY(!PreviewProtocol::validDeadline(std::numeric_limits<qint64>::max(), PreviewProtocol::MaxEnrollmentSeconds));
    QVERIFY(!PreviewProtocol::validDeadline(now - 1, PreviewProtocol::MaxPreviewSeconds));
    QVERIFY(PreviewProtocol::validDeadline(now + 59000, PreviewProtocol::MaxPreviewSeconds));
    QVERIFY(!PreviewProtocol::validDeadline(now + 61000, PreviewProtocol::MaxPreviewSeconds));
    QVERIFY(PreviewProtocol::validDeadline(now + 299000, PreviewProtocol::MaxEnrollmentSeconds));
    QVERIFY(!PreviewProtocol::validDeadline(now + 301000, PreviewProtocol::MaxEnrollmentSeconds));
}

void PreviewProtocolTest::fragmentedRecord()
{
    const QCborMap source{{QStringLiteral("protocol"), PreviewProtocol::Version},
                          {QStringLiteral("type"), QStringLiteral("discover")}};
    const QByteArray bytes = PreviewProtocol::encode(source);
    PreviewProtocol::Parser parser;
    QVector<QCborMap> records;
    QString error;
    QVERIFY(parser.append(QByteArrayView(bytes).first(2), &records, &error));
    QVERIFY(records.isEmpty());
    QVERIFY(parser.append(QByteArrayView(bytes).sliced(2, 3), &records, &error));
    QVERIFY(records.isEmpty());
    QVERIFY(parser.append(QByteArrayView(bytes).sliced(5), &records, &error));
    QCOMPARE(records.size(), 1);
    QCOMPARE(records.constFirst().value(QStringLiteral("type")).toString(), QStringLiteral("discover"));
}

void PreviewProtocolTest::rejectsOversizedRecord()
{
    QByteArray header(4, '\0');
    qToBigEndian(static_cast<quint32>(PreviewProtocol::MaxRecordBytes + 1), header.data());
    PreviewProtocol::Parser parser;
    QVector<QCborMap> records;
    QString error;
    QVERIFY(!parser.append(header, &records, &error));
    QCOMPARE(error, QStringLiteral("protocol-error"));
}

void PreviewProtocolTest::rejectsInvalidCbor()
{
    QByteArray bytes(4, '\0');
    qToBigEndian(static_cast<quint32>(1), bytes.data());
    bytes.append('\xff');
    PreviewProtocol::Parser parser;
    QVector<QCborMap> records;
    QString error;
    QVERIFY(!parser.append(bytes, &records, &error));
    QCOMPARE(error, QStringLiteral("protocol-error"));
}

void PreviewProtocolTest::latestFrameWinsBackpressure()
{
    PreviewProtocol::LatestFrameBuffer buffer;
    QVERIFY(!buffer.hasFrame());
    QVERIFY(!buffer.replace(QByteArrayLiteral("first")));
    QVERIFY(buffer.replace(QByteArrayLiteral("second")));
    QCOMPARE(buffer.take(), QByteArrayLiteral("second"));
    QVERIFY(!buffer.hasFrame());
    buffer.replace(QByteArrayLiteral("third"));
    buffer.clear();
    QVERIFY(!buffer.hasFrame());
}

QTEST_GUILESS_MAIN(PreviewProtocolTest)

#include "test_previewprotocol.moc"
