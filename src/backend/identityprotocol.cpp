// SPDX-License-Identifier: GPL-3.0-or-later

#include "identityprotocol.h"

#include "previewprotocol.h"

#include <QtEndian>

#include <limits>

namespace
{
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

quint16 readU16(QByteArrayView bytes, qsizetype offset)
{
    return qFromBigEndian<quint16>(reinterpret_cast<const uchar *>(bytes.data() + offset));
}

quint64 readU64(QByteArrayView bytes, qsizetype offset)
{
    return qFromBigEndian<quint64>(reinterpret_cast<const uchar *>(bytes.data() + offset));
}

QByteArray request(IdentityProtocol::Operation operation, quint64 generation, quint8 flags, QByteArray data)
{
    if (generation == 0)
    {
        data.fill(0);
        return {};
    }
    QByteArray payload;
    payload.reserve(16 + data.size());
    appendU16(&payload, IdentityProtocol::Version);
    payload.append(static_cast<char>(operation));
    payload.append(static_cast<char>(flags));
    appendU64(&payload, generation);
    appendU32(&payload, IdentityProtocol::DefaultTimeoutMs);
    payload.append(data);
    data.fill(0);

    QByteArray framed;
    framed.reserve(payload.size() + 4);
    appendU32(&framed, static_cast<quint32>(payload.size()));
    framed.append(payload);
    payload.fill(0);
    return framed;
}

QByteArray encodedFrame(const QImage &source, QString *error)
{
    if (source.isNull() || source.width() <= 0 || source.height() <= 0 || source.width() > PreviewProtocol::MaxWidth ||
        source.height() > PreviewProtocol::MaxHeight)
    {
        if (error)
            *error = QStringLiteral("invalid-frame");
        return {};
    }
    QImage frame = source.convertToFormat(QImage::Format_RGB888);
    const qsizetype stride = static_cast<qsizetype>(frame.width()) * 3;
    if (stride <= 0 || stride > std::numeric_limits<quint16>::max())
    {
        frame.fill(0);
        if (error)
            *error = QStringLiteral("invalid-frame");
        return {};
    }
    QByteArray output;
    output.reserve(8 + stride * frame.height());
    output.append(char(1)); // RGB8
    output.append(char(0));
    appendU16(&output, static_cast<quint16>(frame.width()));
    appendU16(&output, static_cast<quint16>(frame.height()));
    appendU16(&output, static_cast<quint16>(stride));
    for (int row = 0; row < frame.height(); ++row)
        output.append(reinterpret_cast<const char *>(frame.constScanLine(row)), stride);
    frame.fill(0);
    return output;
}

bool validKey(const QByteArray &key)
{
    return key.size() == IdentityProtocol::KeyBytes;
}
} // namespace

QByteArray IdentityProtocol::statusRequest(quint64 generation, const QByteArray &key)
{
    if (!key.isEmpty() && !validKey(key))
        return {};
    return request(Operation::Status, generation, key.isEmpty() ? 0 : 1, key);
}

QByteArray IdentityProtocol::extractSampleRequest(quint64 generation, const QByteArray &priorEmbeddings,
                                                  quint8 sampleCount, const QImage &frame, QString *error)
{
    if (sampleCount >= MaximumSamples || priorEmbeddings.size() != sampleCount * EmbeddingBytes)
    {
        if (error)
            *error = QStringLiteral("invalid-sample-state");
        return {};
    }
    QByteArray image = encodedFrame(frame, error);
    if (image.isEmpty())
        return {};
    QByteArray data;
    data.reserve(1 + priorEmbeddings.size() + image.size());
    data.append(static_cast<char>(sampleCount));
    data.append(priorEmbeddings);
    data.append(image);
    image.fill(0);
    return request(Operation::ExtractEnrollmentSample, generation, 0, std::move(data));
}

QByteArray IdentityProtocol::commitRequest(quint64 generation, const QByteArray &key, const QByteArray &embeddings,
                                           quint8 sampleCount)
{
    if (!validKey(key) || sampleCount < 3 || sampleCount > MaximumSamples ||
        embeddings.size() != sampleCount * EmbeddingBytes)
        return {};
    QByteArray data;
    data.reserve(KeyBytes + 1 + embeddings.size());
    data.append(key);
    data.append(static_cast<char>(sampleCount));
    data.append(embeddings);
    return request(Operation::CommitEnrollment, generation, 0, std::move(data));
}

QByteArray IdentityProtocol::summaryRequest(quint64 generation, const QByteArray &key)
{
    if (!validKey(key))
        return {};
    return request(Operation::ListProfileSummary, generation, 0, key);
}

QByteArray IdentityProtocol::verifyRequest(quint64 generation, const QByteArray &key, const QImage &frame,
                                           QString *error)
{
    if (!validKey(key))
    {
        if (error)
            *error = QStringLiteral("vault-key-unavailable");
        return {};
    }
    QByteArray image = encodedFrame(frame, error);
    if (image.isEmpty())
        return {};
    QByteArray data;
    data.reserve(KeyBytes + image.size());
    data.append(key);
    data.append(image);
    image.fill(0);
    return request(Operation::VerifyOneFrame, generation, 0, std::move(data));
}

QByteArray IdentityProtocol::keyRequest(Operation operation, quint64 generation, const QByteArray &key)
{
    if (!validKey(key) || (operation != Operation::DeleteProfile && operation != Operation::ValidateVault))
        return {};
    return request(operation, generation, 0, key);
}

QByteArray IdentityProtocol::resetRequest(quint64 generation)
{
    return request(Operation::ResetUnreadable, generation, 0, {});
}

bool IdentityProtocol::parseResponse(QByteArrayView payload, quint64 expectedGeneration, Response *response,
                                     QString *error)
{
    if (!response || payload.size() < 12 || payload.size() > MaximumResponseBytes || readU16(payload, 0) != Version ||
        readU64(payload, 4) != expectedGeneration)
    {
        if (error)
            *error = QStringLiteral("identity-protocol-error");
        return false;
    }
    const quint8 kind = static_cast<quint8>(payload.at(2));
    const quint8 code = static_cast<quint8>(payload.at(3));
    const qsizetype bodySize = payload.size() - 12;
    const bool valid =
        (kind == static_cast<quint8>(ResponseKind::Status) && code <= 4 && bodySize == 1) ||
        (kind == static_cast<quint8>(ResponseKind::Sample) && code == 0 && bodySize == EmbeddingBytes) ||
        (kind == static_cast<quint8>(ResponseKind::Ack) && code == 0 && bodySize == 0) ||
        (kind == static_cast<quint8>(ResponseKind::Verification) && code >= 1 && code <= 3 && bodySize == 0) ||
        (kind == static_cast<quint8>(ResponseKind::Error) && code >= 1 && code <= 32 && bodySize == 0);
    if (!valid)
    {
        if (error)
            *error = QStringLiteral("identity-protocol-error");
        return false;
    }
    response->kind = static_cast<ResponseKind>(kind);
    response->code = code;
    response->generation = expectedGeneration;
    response->sensitivePayload = QByteArray(payload.sliced(12).data(), bodySize);
    return true;
}
