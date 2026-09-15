// SPDX-License-Identifier: GPL-3.0-or-later

#include "identityprotocol.h"

#include <QImage>
#include <QTest>
#include <QtEndian>

class IdentityProtocolTest final : public QObject
{
    Q_OBJECT

  private Q_SLOTS:
    void requestsAreClosedAndBounded();
    void imageAndEnrollmentBoundsAreEnforced();
    void responsesRequireExactGenerationShapeAndCode();
    void responseDoesNotExposeScores();
};

namespace
{
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

QByteArray response(quint8 kind, quint8 code, quint64 generation, qsizetype bodySize = 0)
{
    QByteArray payload(12 + bodySize, 0);
    qToBigEndian(IdentityProtocol::Version, reinterpret_cast<uchar *>(payload.data()));
    payload[2] = static_cast<char>(kind);
    payload[3] = static_cast<char>(code);
    qToBigEndian(generation, reinterpret_cast<uchar *>(payload.data() + 4));
    return payload;
}
} // namespace

void IdentityProtocolTest::requestsAreClosedAndBounded()
{
    QVERIFY(IdentityProtocol::statusRequest(0).isEmpty());
    QVERIFY(IdentityProtocol::statusRequest(1, QByteArray(31, 1)).isEmpty());

    const QByteArray request = IdentityProtocol::statusRequest(7);
    QCOMPARE(readU32(request, 0), quint32(16));
    QCOMPARE(readU16(request, 4), IdentityProtocol::Version);
    QCOMPARE(static_cast<quint8>(request.at(6)), quint8(IdentityProtocol::Operation::Status));
    QCOMPARE(readU64(request, 8), quint64(7));

    const QByteArray key(IdentityProtocol::KeyBytes, 1);
    QVERIFY(!IdentityProtocol::keyRequest(IdentityProtocol::Operation::DeleteProfile, 8, key).isEmpty());
    QVERIFY(IdentityProtocol::keyRequest(IdentityProtocol::Operation::VerifyOneFrame, 8, key).isEmpty());
}

void IdentityProtocolTest::imageAndEnrollmentBoundsAreEnforced()
{
    QImage valid(640, 480, QImage::Format_RGB888);
    valid.fill(Qt::black);
    QString error;
    const QByteArray key(IdentityProtocol::KeyBytes, 2);
    QVERIFY(!IdentityProtocol::verifyRequest(2, key, valid, &error).isEmpty());

    QImage oversized(1921, 1080, QImage::Format_RGB888);
    QVERIFY(IdentityProtocol::verifyRequest(3, key, oversized, &error).isEmpty());
    QCOMPARE(error, QStringLiteral("invalid-frame"));

    const QByteArray samples(3 * IdentityProtocol::EmbeddingBytes, 0);
    QVERIFY(!IdentityProtocol::commitRequest(4, key, samples, 3).isEmpty());
    QVERIFY(IdentityProtocol::commitRequest(4, key, samples, 2).isEmpty());
    QVERIFY(IdentityProtocol::commitRequest(
                4, key, QByteArray((IdentityProtocol::MaximumSamples + 1) * IdentityProtocol::EmbeddingBytes, 0),
                IdentityProtocol::MaximumSamples + 1)
                .isEmpty());
}

void IdentityProtocolTest::responsesRequireExactGenerationShapeAndCode()
{
    IdentityProtocol::Response parsed;
    QString error;
    const QByteArray status = response(static_cast<quint8>(IdentityProtocol::ResponseKind::Status), 1, 9, 1);
    QVERIFY(IdentityProtocol::parseResponse(status, 9, &parsed, &error));
    QCOMPARE(parsed.kind, IdentityProtocol::ResponseKind::Status);
    QCOMPARE(parsed.sensitivePayload.size(), qsizetype(1));

    QVERIFY(!IdentityProtocol::parseResponse(status, 10, &parsed, &error));
    QVERIFY(!IdentityProtocol::parseResponse(
        response(static_cast<quint8>(IdentityProtocol::ResponseKind::Verification), 4, 9), 9, &parsed, &error));
    QVERIFY(!IdentityProtocol::parseResponse(response(static_cast<quint8>(IdentityProtocol::ResponseKind::Sample), 0, 9,
                                                      IdentityProtocol::EmbeddingBytes - 1),
                                             9, &parsed, &error));
}

void IdentityProtocolTest::responseDoesNotExposeScores()
{
    IdentityProtocol::Response parsed;
    QString error;
    for (quint8 result = 1; result <= 3; ++result)
    {
        const QByteArray payload =
            response(static_cast<quint8>(IdentityProtocol::ResponseKind::Verification), result, 11);
        QCOMPARE(payload.size(), qsizetype(12));
        QVERIFY(IdentityProtocol::parseResponse(payload, 11, &parsed, &error));
        QVERIFY(parsed.sensitivePayload.isEmpty());
    }
}

QTEST_GUILESS_MAIN(IdentityProtocolTest)

#include "test_identityprotocol.moc"
