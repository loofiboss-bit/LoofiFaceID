// SPDX-License-Identifier: GPL-3.0-or-later

#include "sddmthemeconfig.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return {false, false, {}};
    struct stat metadata{};
    if (::fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode))
    {
        ::close(fd);
        return {false, false, {}};
    }
    QFile file;
    if (!file.open(fd, QIODevice::ReadOnly | QIODevice::Text, QFileDevice::AutoCloseHandle))
    {
        ::close(fd);
        return {false, false, {}};
    }

    if (file.size() > 65536)
        return {false, false, {}};
    QString activeSection;
    IniValue result;
    while (!file.atEnd())
    {
        const QString line = QString::fromUtf8(file.readLine(65537)).trimmed();
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
    if (file.error() != QFileDevice::NoError)
        return {false, false, {}};
    return result;
}

bool readConfigFile(const QString &path, QString &currentTheme, QString &themeDirectory)
{
    struct stat metadata{};
    if (::stat(QFile::encodeName(path).constData(), &metadata) != 0)
        return errno == ENOENT;

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
    struct stat metadata{};
    if (::stat(QFile::encodeName(directory).constData(), &metadata) != 0)
        return errno == ENOENT;
    if (!S_ISDIR(metadata.st_mode) || ::access(QFile::encodeName(directory).constData(), R_OK | X_OK) != 0)
        return false;
    const QDir configDirectory(directory);
    const QStringList files = configDirectory.entryList({QStringLiteral("*.conf")}, QDir::Files, QDir::LocaleAware);
    for (const QString &file : files)
    {
        if (!readConfigFile(configDirectory.filePath(file), currentTheme, themeDirectory))
            return false;
    }
    return true;
}

KFaceAuth::SddmThemeSupport themeDeclaresFaceAuthentication(const QString &themeDirectory, const QString &themeName)
{
    const QDir activeTheme(QDir(themeDirectory).filePath(themeName));
    const IniValue configFileValue = readIniValue(activeTheme.filePath(QStringLiteral("metadata.desktop")),
                                                  QStringLiteral("SddmGreeterTheme"), QStringLiteral("ConfigFile"));
    if (!configFileValue.valid)
        return KFaceAuth::SddmThemeSupport::Unknown;

    const QString configFile = configFileValue.present ? configFileValue.value : QStringLiteral("theme.conf");
    if (configFile.isEmpty() || QDir::isAbsolutePath(configFile))
        return KFaceAuth::SddmThemeSupport::Unsupported;
    const QString cleanConfigFile = QDir::cleanPath(configFile);
    if (cleanConfigFile == QLatin1String("..") || cleanConfigFile.startsWith(QLatin1String("../")))
        return KFaceAuth::SddmThemeSupport::Unsupported;

    const IniValue interfaceVersion = readIniValue(activeTheme.filePath(cleanConfigFile), QStringLiteral("General"),
                                                   QStringLiteral("faceAuthenticationApi"));
    if (!interfaceVersion.valid)
        return KFaceAuth::SddmThemeSupport::Unknown;
    return interfaceVersion.present && interfaceVersion.value == QLatin1String("1")
               ? KFaceAuth::SddmThemeSupport::Declared
               : KFaceAuth::SddmThemeSupport::Unsupported;
}
} // namespace

namespace KFaceAuth
{
SddmThemeSupport activeSddmThemeSupportsFaceAuthentication(const SddmThemeConfigPaths &paths)
{
    QString currentTheme;
    QString themeDirectory = paths.defaultThemeDirectory;

    for (const QString &directory : paths.systemConfigDirectories)
    {
        if (!readDirectory(directory, currentTheme, themeDirectory))
            return SddmThemeSupport::Unknown;
    }
    for (const QString &directory : paths.localConfigDirectories)
    {
        if (!readDirectory(directory, currentTheme, themeDirectory))
            return SddmThemeSupport::Unknown;
    }
    if (!readConfigFile(paths.mainConfigFile, currentTheme, themeDirectory))
        return SddmThemeSupport::Unknown;

    // The patched SDDM build embeds the v1 control in its fallback theme.
    if (currentTheme.isEmpty())
        return SddmThemeSupport::Declared;
    if (themeDirectory.isEmpty())
        return SddmThemeSupport::Unknown;
    return themeDeclaresFaceAuthentication(themeDirectory, currentTheme);
}
} // namespace KFaceAuth
