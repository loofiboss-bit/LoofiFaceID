// SPDX-License-Identifier: GPL-3.0-or-later

#include "systemauthprotocol.h"

#include <QtEndian>

namespace
{
constexpr quint16 Version = 2;
constexpr quint8 StatusSuccess = 0;
constexpr quint8 StatusAccessDenied = 2;
constexpr quint8 StatusNoProfile = 3;
constexpr quint8 StatusTimeout = 4;
constexpr quint8 StatusDeviceBusy = 5;
constexpr quint8 StatusInternalError = 6;
constexpr quint8 StatusSpoofDetected = 7;
constexpr quint8 StatusRateLimited = 8;
constexpr quint8 StatusCancelled = 9;
constexpr quint8 MaximumSamples = 8;
constexpr quint8 MinimumSamples = 3;
constexpr qsizetype RequestPayloadSize = 8;
constexpr qsizetype ResponseHeaderSize = 4;
constexpr qsizetype StatusResponseSize = 6;

bool isKnownFailureStatus(quint8 status)
{
    return status == StatusAccessDenied || status == StatusNoProfile || status == StatusTimeout ||
           status == StatusDeviceBusy || status == StatusInternalError || status == StatusSpoofDetected ||
           status == StatusRateLimited || status == StatusCancelled;
}
} // namespace

QByteArray SystemAuthProtocol::statusRequest(quint32 targetUid)
{
    QByteArray request(4 + RequestPayloadSize, Qt::Uninitialized);
    qToBigEndian(static_cast<quint32>(RequestPayloadSize), reinterpret_cast<uchar *>(request.data()));
    qToBigEndian(Version, reinterpret_cast<uchar *>(request.data() + 4));
    request[6] = char(0x11);
    request[7] = 0;
    qToBigEndian(targetUid, reinterpret_cast<uchar *>(request.data() + 8));
    return request;
}

SystemAuthProtocol::Status SystemAuthProtocol::parseStatusResponse(QByteArrayView payload)
{
    Status result;
    if ((payload.size() != ResponseHeaderSize && payload.size() != StatusResponseSize) ||
        qFromBigEndian<quint16>(reinterpret_cast<const uchar *>(payload.data())) != Version || payload.at(3) != 0)
        return result;

    const quint8 statusCode = static_cast<quint8>(payload.at(2));
    if (statusCode == StatusSuccess)
    {
        if (payload.size() != StatusResponseSize)
            return result;
        const quint8 enrolled = static_cast<quint8>(payload.at(4));
        const quint8 sampleCount = static_cast<quint8>(payload.at(5));
        if (enrolled > 1 || sampleCount > MaximumSamples || (enrolled == 0 && sampleCount != 0) ||
            (enrolled == 1 && sampleCount < MinimumSamples))
            return result;

        result.daemonReady = true;
        result.systemProfileReady = enrolled == 1;
        result.systemProfileKnown = true;
        return result;
    }

    if (!isKnownFailureStatus(statusCode))
        return result;
    if (payload.size() == StatusResponseSize &&
        (statusCode != StatusNoProfile || payload.at(4) != 0 || payload.at(5) != 0))
        return result;

    result.daemonReady = true;
    result.systemProfileKnown = statusCode == StatusNoProfile;
    return result;
}
