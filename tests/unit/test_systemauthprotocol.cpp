// SPDX-License-Identifier: GPL-3.0-or-later

#include "systemauthprotocol.h"

#include <QTest>
#include <QtEndian>

class SystemAuthProtocolTest final : public QObject
{
    Q_OBJECT

  private Q_SLOTS:
    void statusRequestTargetsTheRequestedUid();
    void statusResponseSeparatesDaemonAndProfileReadiness();
    void malformedStatusResponsesFailClosed();
    void freshnessIsVersionedAndFailsClosed();
};

namespace
{
QByteArray response(quint8 code, std::initializer_list<quint8> body = {})
{
    QByteArray payload(4 + static_cast<qsizetype>(body.size()), Qt::Uninitialized);
    qToBigEndian(quint16(2), reinterpret_cast<uchar *>(payload.data()));
    payload[2] = static_cast<char>(code);
    payload[3] = 0;
    qsizetype offset = 4;
    for (const quint8 value : body)
        payload[offset++] = static_cast<char>(value);
    return payload;
}
} // namespace

void SystemAuthProtocolTest::statusRequestTargetsTheRequestedUid()
{
    const QByteArray request = SystemAuthProtocol::statusRequest(1000);

    QCOMPARE(qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(request.constData())), quint32(8));
    QCOMPARE(qFromBigEndian<quint16>(reinterpret_cast<const uchar *>(request.constData() + 4)), quint16(2));
    QCOMPARE(static_cast<quint8>(request.at(6)), quint8(0x11));
    QCOMPARE(static_cast<quint8>(request.at(7)), quint8(0));
    QCOMPARE(qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(request.constData() + 8)), quint32(1000));
}

void SystemAuthProtocolTest::statusResponseSeparatesDaemonAndProfileReadiness()
{
    const auto ready = SystemAuthProtocol::parseStatusResponse(response(0, {1, 5}));
    QVERIFY(ready.daemonReady);
    QVERIFY(ready.systemProfileReady);
    QVERIFY(ready.systemProfileKnown);

    const auto absent = SystemAuthProtocol::parseStatusResponse(response(0, {0, 0}));
    QVERIFY(absent.daemonReady);
    QVERIFY(!absent.systemProfileReady);
    QVERIFY(absent.systemProfileKnown);

    const auto missing = SystemAuthProtocol::parseStatusResponse(response(3));
    QVERIFY(missing.daemonReady);
    QVERIFY(!missing.systemProfileReady);

    const auto unavailableKey = SystemAuthProtocol::parseStatusResponse(response(6));
    QVERIFY(unavailableKey.daemonReady);
    QVERIFY(!unavailableKey.systemProfileReady);
    QVERIFY(!unavailableKey.systemProfileKnown);
}

void SystemAuthProtocolTest::malformedStatusResponsesFailClosed()
{
    QByteArray wrongVersion = response(0, {1, 5});
    qToBigEndian(quint16(1), reinterpret_cast<uchar *>(wrongVersion.data()));
    QVERIFY(!SystemAuthProtocol::parseStatusResponse(wrongVersion).daemonReady);

    QVERIFY(!SystemAuthProtocol::parseStatusResponse(response(0)).daemonReady);
    QVERIFY(!SystemAuthProtocol::parseStatusResponse(response(0, {1, 9})).daemonReady);
    QVERIFY(!SystemAuthProtocol::parseStatusResponse(response(0, {1, 2})).daemonReady);
    QVERIFY(!SystemAuthProtocol::parseStatusResponse(response(0, {0, 1})).daemonReady);
    QVERIFY(SystemAuthProtocol::parseStatusResponse(response(9)).daemonReady);
}

void SystemAuthProtocolTest::freshnessIsVersionedAndFailsClosed()
{
    const QByteArray digest(32, char(0xa5));
    const auto request = SystemAuthProtocol::freshnessRequest(1000, digest);
    QCOMPARE(request.size(), 44);
    QCOMPARE(qFromBigEndian<quint16>(reinterpret_cast<const uchar *>(request.constData() + 4)), quint16(3));
    QCOMPARE(request.mid(12), digest);
    QVERIFY(SystemAuthProtocol::freshnessRequest(1000, QByteArray(31, 'x')).isEmpty());
    using Freshness = SystemAuthProtocol::ProfileFreshness;
    QCOMPARE(SystemAuthProtocol::parseFreshnessResponse(QByteArray::fromHex("0003000001")), Freshness::Current);
    QCOMPARE(SystemAuthProtocol::parseFreshnessResponse(QByteArray::fromHex("0003000002")), Freshness::Stale);
    for (const auto &invalid : {"0002000001", "0003020001", "0003000101", "0003000003", "00030000", "000300000100"})
        QCOMPARE(SystemAuthProtocol::parseFreshnessResponse(QByteArray::fromHex(invalid)), Freshness::Unknown);
}

QTEST_GUILESS_MAIN(SystemAuthProtocolTest)

#include "test_systemauthprotocol.moc"
