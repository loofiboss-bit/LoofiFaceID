// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QString>
#include <QStringList>

namespace KFaceAuth
{
struct SddmThemeConfigPaths
{
    QStringList systemConfigDirectories;
    QStringList localConfigDirectories;
    QString mainConfigFile;
    QString defaultThemeDirectory;
};

enum class SddmThemeSupport
{
    Unknown,
    Unsupported,
    Declared
};

// Reads SDDM's layered configuration and checks the active theme's declared
// interface. An empty Current selects SDDM's patched embedded fallback theme.
[[nodiscard]] SddmThemeSupport activeSddmThemeSupportsFaceAuthentication(const SddmThemeConfigPaths &paths);
} // namespace KFaceAuth
