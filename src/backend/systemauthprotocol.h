// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QtTypes>

namespace SystemAuthProtocol
{
enum class ProfileFreshness : quint8
{
    Unknown = 0,
    Current = 1,
    Stale = 2
};

struct Status
{
    bool daemonReady = false;
    bool systemProfileReady = false;
    bool systemProfileKnown = false;
};

// Backend-private digest input; only the aggregate enum is exposed to UI.
[[nodiscard]] QByteArray freshnessRequest(quint32 targetUid, QByteArrayView ciphertextSha256);
[[nodiscard]] ProfileFreshness parseFreshnessResponse(QByteArrayView payload);

[[nodiscard]] QByteArray statusRequest(quint32 targetUid);
[[nodiscard]] Status parseStatusResponse(QByteArrayView payload);
} // namespace SystemAuthProtocol
