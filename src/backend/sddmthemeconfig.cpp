// SPDX-License-Identifier: GPL-3.0-or-later

#include "sddmthemeconfig.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace
{
struct IniValue
{
    bool valid = true;
    bool present = false;
    QString value;
};

IniValue readIniValue(const QString &path, const QString &section, const QString &key)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {false, false, {}};

    QString activeSection;
    IniValue result;
    while (!file.atEnd())
    {
        const QString line = QString::fromUtf8(file.readLine()).trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#')) || line.startsWith(QLatin1Char(';')))
            continue;
        if (line.startsWith(QLatin1Char('[')) && line.endsWith(QLatin1Char(']')))
        {
            activeSection = line.sliced(1, line.size() - 2).trimmed();
            continue;
        }
        if (activeSection != section && !(section == QLatin1String("General") && activeSection.isEmpty()))
            continue;

        const qsizetype separator = line.indexOf(QLatin1Char('='));
        if (separator <= 0 || line.first(separator).trimmed() != key)
            continue;
        if (result.present)
            return {false, false, {}};
        result.present = true;
        result.value = line.sliced(separator + 1).trimmed();
    }
    return result;
}

bool readConfigFile(const QString &path, QString &currentTheme, QString &themeDirectory)
{
    if (!QFileInfo::exists(path))
        return true;

    const IniValue current = readIniValue(path, QStringLiteral("Theme"), QStringLiteral("Current"));
    const IniValue directory = readIniValue(path, QStringLiteral("Theme"), QStringLiteral("ThemeDir"));
    if (!current.valid || !directory.valid)
        return false;
    if (current.present)
        currentTheme = current.value;
    if (directory.present)
        themeDirectory = directory.value;
    return true;
}

bool readDirectory(const QString &directory, QString &currentTheme, QString &themeDirectory)
{
    const QDir configDirectory(directory);
    const QStringList files = configDirectory.entryList({QStringLiteral("*.conf")}, QDir::Files, QDir::LocaleAware);
    for (const QString &file : files)
    {
        if (!readConfigFile(configDirectory.filePath(file), currentTheme, themeDirectory))
            return false;
    }
    return true;
}

bool themeDeclaresFaceAuthentication(const QString &themeDirectory, const QString &themeName)
{
    const QDir activeTheme(QDir(themeDirectory).filePath(themeName));
    const IniValue configFileValue = readIniValue(activeTheme.filePath(QStringLiteral("metadata.desktop")),
                                                  QStringLiteral("SddmGreeterTheme"), QStringLiteral("ConfigFile"));
    if (!configFileValue.valid)
        return false;

    const QString configFile = configFileValue.present ? configFileValue.value : QStringLiteral("theme.conf");
    if (configFile.isEmpty() || QDir::isAbsolutePath(configFile))
        return false;
    const QString cleanConfigFile = QDir::cleanPath(configFile);
    if (cleanConfigFile == QLatin1String("..") || cleanConfigFile.startsWith(QLatin1String("../")))
        return false;

    const IniValue interfaceVersion = readIniValue(activeTheme.filePath(cleanConfigFile), QStringLiteral("General"),
                                                   QStringLiteral("faceAuthenticationApi"));
    return interfaceVersion.valid && interfaceVersion.present && interfaceVersion.value == QLatin1String("1");
}
} // namespace

namespace KFaceAuth
{
bool activeSddmThemeSupportsFaceAuthentication(const SddmThemeConfigPaths &paths)
{
    QString currentTheme;
    QString themeDirectory = paths.defaultThemeDirectory;

    for (const QString &directory : paths.systemConfigDirectories)
    {
        if (!readDirectory(directory, currentTheme, themeDirectory))
            return false;
    }
    for (const QString &directory : paths.localConfigDirectories)
    {
        if (!readDirectory(directory, currentTheme, themeDirectory))
            return false;
    }
    if (!readConfigFile(paths.mainConfigFile, currentTheme, themeDirectory))
        return false;

    // The patched SDDM build embeds the v1 control in its fallback theme.
    if (currentTheme.isEmpty())
        return true;
    if (themeDirectory.isEmpty())
        return false;
    return themeDeclaresFaceAuthentication(themeDirectory, currentTheme);
}
} // namespace KFaceAuth
