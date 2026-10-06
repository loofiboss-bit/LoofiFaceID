// SPDX-License-Identifier: BSD-2-Clause

#include "sddmthemeconfig.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

using KFaceAuth::SddmThemeConfigPaths;

class TestSddmThemeConfig final : public QObject
{
    Q_OBJECT

  private Q_SLOTS:
    void activeThemeMustDeclareTheVersionedInterface();
    void effectiveConfigurationUsesSddmPrecedence();
    void metadataMayChooseThemeConfigurationFile();
    void emptyThemeUsesEmbeddedFallback();
    void unsupportedOrEscapingThemeConfigurationFailsClosed();
};

static bool writeFile(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(contents) == contents.size();
}

static SddmThemeConfigPaths pathsFor(const QString &root)
{
    const QString base = root + QStringLiteral("/");
    return {{base + QStringLiteral("system")},
            {base + QStringLiteral("local")},
            base + QStringLiteral("sddm.conf"),
            base + QStringLiteral("themes")};
}

void TestSddmThemeConfig::activeThemeMustDeclareTheVersionedInterface()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto paths = pathsFor(temporary.path());
    QVERIFY(QDir().mkpath(paths.defaultThemeDirectory + QStringLiteral("/face")));
    QVERIFY(writeFile(paths.defaultThemeDirectory + QStringLiteral("/face/metadata.desktop"),
                      "[SddmGreeterTheme]\nConfigFile=theme.conf\n"));
    QVERIFY(writeFile(paths.defaultThemeDirectory + QStringLiteral("/face/theme.conf"),
                      "[General]\nfaceAuthenticationApi=1\n"));
    QVERIFY(writeFile(paths.mainConfigFile, "[Theme]\nCurrent=face\n"));

    QVERIFY(KFaceAuth::activeSddmThemeSupportsFaceAuthentication(paths));
    QVERIFY(writeFile(paths.defaultThemeDirectory + QStringLiteral("/face/theme.conf"),
                      "[General]\nfaceAuthenticationApi=2\n"));
    QVERIFY(!KFaceAuth::activeSddmThemeSupportsFaceAuthentication(paths));
}

void TestSddmThemeConfig::effectiveConfigurationUsesSddmPrecedence()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto paths = pathsFor(temporary.path());
    QVERIFY(QDir().mkpath(paths.defaultThemeDirectory + QStringLiteral("/face")));
    QVERIFY(QDir().mkpath(paths.defaultThemeDirectory + QStringLiteral("/plain")));
    QVERIFY(writeFile(paths.defaultThemeDirectory + QStringLiteral("/face/metadata.desktop"), "[SddmGreeterTheme]\n"));
    QVERIFY(writeFile(paths.defaultThemeDirectory + QStringLiteral("/face/theme.conf"),
                      "[General]\nfaceAuthenticationApi=1\n"));
    QVERIFY(writeFile(paths.defaultThemeDirectory + QStringLiteral("/plain/metadata.desktop"), "[SddmGreeterTheme]\n"));
    QVERIFY(writeFile(paths.defaultThemeDirectory + QStringLiteral("/plain/theme.conf"), "[General]\n"));
    QVERIFY(QDir().mkpath(paths.systemConfigDirectories.constFirst()));
    QVERIFY(QDir().mkpath(paths.localConfigDirectories.constFirst()));
    QVERIFY(writeFile(paths.systemConfigDirectories.constFirst() + QStringLiteral("/10-theme.conf"),
                      "[Theme]\nCurrent=plain\n"));
    QVERIFY(writeFile(paths.localConfigDirectories.constFirst() + QStringLiteral("/20-theme.conf"),
                      "[Theme]\nCurrent=face\n"));
    QVERIFY(writeFile(paths.mainConfigFile, "[Theme]\nCurrent=plain\n"));

    QVERIFY(!KFaceAuth::activeSddmThemeSupportsFaceAuthentication(paths));
    QVERIFY(writeFile(paths.mainConfigFile, "[Theme]\nCurrent=face\n"));
    QVERIFY(KFaceAuth::activeSddmThemeSupportsFaceAuthentication(paths));
}

void TestSddmThemeConfig::metadataMayChooseThemeConfigurationFile()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto paths = pathsFor(temporary.path());
    const QString theme = paths.defaultThemeDirectory + QStringLiteral("/face");
    QVERIFY(QDir().mkpath(theme + QStringLiteral("/settings")));
    QVERIFY(
        writeFile(theme + QStringLiteral("/metadata.desktop"), "[SddmGreeterTheme]\nConfigFile=settings/face.conf\n"));
    QVERIFY(writeFile(theme + QStringLiteral("/settings/face.conf"), "[General]\nfaceAuthenticationApi=1\n"));
    QVERIFY(writeFile(paths.mainConfigFile, "[Theme]\nCurrent=face\n"));

    QVERIFY(KFaceAuth::activeSddmThemeSupportsFaceAuthentication(paths));
}

void TestSddmThemeConfig::emptyThemeUsesEmbeddedFallback()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto paths = pathsFor(temporary.path());
    QVERIFY(writeFile(paths.mainConfigFile, "[Theme]\nCurrent=\n"));

    QVERIFY(KFaceAuth::activeSddmThemeSupportsFaceAuthentication(paths));
}

void TestSddmThemeConfig::unsupportedOrEscapingThemeConfigurationFailsClosed()
{
    QTemporaryDir temporary;
    QVERIFY(temporary.isValid());
    const auto paths = pathsFor(temporary.path());
    const QString theme = paths.defaultThemeDirectory + QStringLiteral("/face");
    QVERIFY(QDir().mkpath(theme));
    QVERIFY(
        writeFile(theme + QStringLiteral("/metadata.desktop"), "[SddmGreeterTheme]\nConfigFile=../../outside.conf\n"));
    QVERIFY(writeFile(paths.mainConfigFile, "[Theme]\nCurrent=face\n"));

    QVERIFY(!KFaceAuth::activeSddmThemeSupportsFaceAuthentication(paths));
    QVERIFY(writeFile(theme + QStringLiteral("/metadata.desktop"), "[SddmGreeterTheme]\n"));
    QVERIFY(writeFile(theme + QStringLiteral("/theme.conf"), "[General]\nfaceAuthenticationApi=\n"));
    QVERIFY(!KFaceAuth::activeSddmThemeSupportsFaceAuthentication(paths));
}

QTEST_GUILESS_MAIN(TestSddmThemeConfig)

#include "test_sddmthemeconfig.moc"
