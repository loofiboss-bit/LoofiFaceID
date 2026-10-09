// SPDX-License-Identifier: GPL-3.0-or-later
#include "authcameraconfiguration.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

class AuthCameraConfigurationTest final : public QObject
{
    Q_OBJECT
  private Q_SLOTS:
    void refreshAndApplyExposeOnlyLabelsAndTokens();
    void failedAuthorizationAndMalformedReadback();
};

namespace
{
QString script(QTemporaryDir &directory, const QString &name, const QByteArray &body)
{
    const QString path = directory.filePath(name);
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write("#!/bin/sh\n" + body) < 0)
        return {};
    file.close();
    if (!file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner))
        return {};
    return path;
}
QByteArray list(const QByteArray &selection)
{
    return "KFCAMERA1\n" + selection + "\ndevice " + QByteArray("/dev/video1").toHex() + ' ' +
           QByteArray("Camera").toHex() + "\n";
}
} // namespace

void AuthCameraConfigurationTest::refreshAndApplyExposeOnlyLabelsAndTokens()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QByteArray automatic = list("automatic");
    const QByteArray selected = list("selected " + QByteArray("/dev/video1").toHex());
    const QString helper =
        script(directory, QStringLiteral("helper"),
               "if [ \"$1\" = --list ]; then printf '" + automatic + "'; elif [ \"$1\" = --reset ]; then printf '" +
                   automatic + "'; else read selected; [ \"$selected\" = /dev/video1 ] || exit 1; printf '" + selected +
                   "'; fi\n");
    const QString authorization = script(directory, QStringLiteral("authorization"), "exec \"$@\"\n");
    AuthCameraConfiguration camera(helper, authorization, nullptr);
    camera.refresh();
    QTRY_VERIFY(!camera.busy());
    QCOMPARE(camera.state(), AuthCameraConfiguration::State::Automatic);
    QCOMPARE(camera.devices().size(), 1);
    const QVariantMap device = camera.devices().first().toMap();
    QCOMPARE(device.value(QStringLiteral("label")).toString(), QStringLiteral("Camera"));
    const QString token = device.value(QStringLiteral("token")).toString();
    QVERIFY(!token.isEmpty());
    QVERIFY(!token.contains(QLatin1String("/dev")));
    QVERIFY(!device.contains(QStringLiteral("path")));
    camera.apply(token);
    QTRY_VERIFY(!camera.busy());
    QCOMPARE(camera.state(), AuthCameraConfiguration::State::Selected);
    QVERIFY(!camera.selectedToken().isEmpty());
    QVERIFY(camera.message().contains(QStringLiteral("verified")));
    QVERIFY(camera.selectionText().contains(QStringLiteral("Camera")));
    QVERIFY(!camera.selectionText().contains(QStringLiteral("/dev/")));
    camera.reset();
    QTRY_VERIFY(!camera.busy());
    QCOMPARE(camera.state(), AuthCameraConfiguration::State::Automatic);
    camera.apply(token); // Refresh/apply invalidates old opaque tokens.
    QCOMPARE(camera.state(), AuthCameraConfiguration::State::Failed);
    QVERIFY(!camera.busy());
}

void AuthCameraConfigurationTest::failedAuthorizationAndMalformedReadback()
{
    QTemporaryDir directory;
    const QString helper = script(directory, QStringLiteral("helper"), "printf '" + list("automatic") + "'\n");
    const QString denied = script(directory, QStringLiteral("denied"), "exit 126\n");
    AuthCameraConfiguration camera(helper, denied, nullptr);
    camera.refresh();
    QTRY_VERIFY(!camera.busy());
    QCOMPARE(camera.state(), AuthCameraConfiguration::State::Failed);
    QVERIFY(camera.devices().isEmpty());
    camera.reset();
    QTRY_VERIFY(!camera.busy());
    QCOMPARE(camera.state(), AuthCameraConfiguration::State::Failed);
    QVERIFY(camera.message().contains(QStringLiteral("not authorized")));
    const QString malformed =
        script(directory, QStringLiteral("malformed"), "printf 'KFCAMERA1\\nselected 2f6574632f706173737764\\n'\n");
    const QString allowed = script(directory, QStringLiteral("allowed"), "exec \"$@\"\n");
    AuthCameraConfiguration invalid(malformed, allowed, nullptr);
    invalid.refresh();
    QTRY_VERIFY(!invalid.busy());
    QCOMPARE(invalid.state(), AuthCameraConfiguration::State::Failed);
    QVERIFY(invalid.devices().isEmpty());
}

QTEST_GUILESS_MAIN(AuthCameraConfigurationTest)
#include "test_authcameraconfiguration.moc"
