// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QByteArrayView>
#include <QtTypes>

namespace SystemAuthProtocol
{
struct Status
{
    bool daemonReady = false;
    bool systemProfileReady = false;
    bool systemProfileKnown = false;
};

[[nodiscard]] QByteArray statusRequest(quint32 targetUid);
[[nodiscard]] Status parseStatusResponse(QByteArrayView payload);
} // namespace SystemAuthProtocol
