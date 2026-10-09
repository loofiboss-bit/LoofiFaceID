// SPDX-License-Identifier: GPL-3.0-or-later
#include "authcameraconfiguration.h"

#include <KLocalizedString>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QSet>
#include <QUuid>

namespace
{
QString translate(const char *text)
{
    return i18nd("kcm_kfaceauth", text);
}
constexpr qsizetype MaxOutput = 65536;
QByteArray decode(const QByteArray &hex)
{
    if (hex.isEmpty() || hex.size() % 2 != 0 || hex.size() > 8192)
        return {};
    for (const char byte : hex)
    {
        if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
            return {};
    }
    return QByteArray::fromHex(hex);
}
bool validDevicePath(const QByteArray &path)
{
    if (path.contains('\0') || path.contains('\n') || path.contains('\r'))
        return false;
    if (path.startsWith("/dev/v4l/by-path/") || path.startsWith("/dev/v4l/by-id/"))
    {
        const auto name = path.mid(path.startsWith("/dev/v4l/by-id/") ? 15 : 17);
        return !name.isEmpty() && !name.contains('/') && name != "." && name != "..";
    }
    if (!path.startsWith("/dev/video"))
        return false;
    const auto number = path.mid(10);
    if (number.isEmpty())
        return false;
    for (const auto byte : number)
        if (byte < '0' || byte > '9')
            return false;
    return true;
}
} // namespace

AuthCameraConfiguration::AuthCameraConfiguration(QObject *parent)
    : AuthCameraConfiguration(QStringLiteral("/usr/libexec/kfaceauth-camera-config"), QStringLiteral("/usr/bin/pkexec"),
                              parent)
{
}
AuthCameraConfiguration::AuthCameraConfiguration(QString helperPath, QString authorizationPath, QObject *parent)
    : QObject(parent), m_helperPath(std::move(helperPath)), m_authorizationPath(std::move(authorizationPath)),
      m_available(QFileInfo(m_helperPath).isExecutable())
{
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this,
            [this]
            {
                m_timedOut = true;
                m_process.kill();
            });
    connect(&m_process, &QProcess::readyReadStandardOutput, this,
            [this]
            {
                m_output += m_process.readAllStandardOutput();
                if (m_output.size() > MaxOutput)
                {
                    m_timedOut = true;
                    m_process.kill();
                }
            });
    connect(&m_process, &QProcess::readyReadStandardError, this,
            [this]
            {
                m_errors += m_process.readAllStandardError();
                if (m_errors.size() > MaxOutput)
                {
                    m_timedOut = true;
                    m_process.kill();
                }
            });
    connect(&m_process, &QProcess::started, this,
            [this]
            {
                if (!m_input.isEmpty())
                    m_process.write(m_input);
                m_process.closeWriteChannel();
            });
    connect(&m_process, &QProcess::finished, this, &AuthCameraConfiguration::finished);
    connect(&m_process, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error)
            {
                if (error == QProcess::FailedToStart)
                {
                    m_timeout.stop();
                    fail(translate("Camera configuration helper is unavailable."));
                }
            });
}
bool AuthCameraConfiguration::available() const
{
    return m_available;
}
bool AuthCameraConfiguration::busy() const
{
    return m_process.state() != QProcess::NotRunning;
}
QVariantList AuthCameraConfiguration::devices() const
{
    return m_devices;
}
QString AuthCameraConfiguration::selectedToken() const
{
    return m_selectedToken;
}
AuthCameraConfiguration::State AuthCameraConfiguration::state() const
{
    return m_state;
}
QString AuthCameraConfiguration::message() const
{
    return m_message;
}
QString AuthCameraConfiguration::selectionText() const
{
    if (m_state == State::Automatic)
        return translate("Automatic camera selection");
    if (m_state == State::Selected)
    {
        for (const auto &entry : m_devices)
        {
            const auto device = entry.toMap();
            if (device.value(QStringLiteral("token")).toString() == m_selectedToken)
                return i18nd("kcm_kfaceauth", "Configured camera: %1",
                             device.value(QStringLiteral("label")).toString());
        }
    }
    return translate("Configured camera is unknown.");
}

void AuthCameraConfiguration::fail(const QString &message)
{
    m_state = State::Failed;
    m_message = message;
    Q_EMIT changed();
}
void AuthCameraConfiguration::refresh()
{
    if (busy())
        return;
    start(false, QStringLiteral("--list"));
}
void AuthCameraConfiguration::apply(const QString &token)
{
    if (busy())
        return;
    if (!m_paths.contains(token))
    {
        fail(translate("The selected camera is no longer available. Refresh the camera list."));
        return;
    }
    m_expected = m_paths.value(token);
    start(true, QStringLiteral("--set"), m_expected + '\n');
}
void AuthCameraConfiguration::reset()
{
    if (busy())
        return;
    m_expected.clear();
    start(true, QStringLiteral("--reset"));
}
void AuthCameraConfiguration::start(bool mutation, const QString &operation, const QByteArray &input)
{
    if (!m_available)
    {
        fail(translate("Camera configuration helper is unavailable."));
        return;
    }
    m_mutation = mutation;
    m_timedOut = false;
    m_output.clear();
    m_errors.clear();
    m_input = input;
    m_message = mutation ? translate("Applying camera setting and restarting the authentication service…")
                         : translate("Checking compatible cameras…");
    QProcessEnvironment environment;
    environment.insert(QStringLiteral("LANG"), QStringLiteral("C.UTF-8"));
    m_process.setProcessEnvironment(environment);
    m_process.setProgram(m_authorizationPath);
    m_process.setArguments({m_helperPath, operation});
    m_process.start();
    // Authentication may take time; helper service operations have their own bounds.
    m_timeout.start(180000);
    Q_EMIT changed();
}
void AuthCameraConfiguration::finished(int exitCode, QProcess::ExitStatus exitStatus)
{
    m_timeout.stop();
    m_output += m_process.readAllStandardOutput();
    m_errors += m_process.readAllStandardError();
    if (m_timedOut || m_output.size() > MaxOutput || m_errors.size() > MaxOutput)
    {
        fail(translate("Camera configuration timed out. Refresh to verify the setting."));
        return;
    }
    if (exitStatus != QProcess::NormalExit || exitCode != 0)
    {
        if (exitCode == 126 || exitCode == 127 || m_errors.contains("error unauthorized"))
            fail(translate("Camera configuration was not authorized."));
        else if (m_errors.contains("error rollback-failed"))
            fail(translate(
                "Camera configuration failed and the previous service state could not be restored. Open Diagnostics."));
        else
            fail(translate("Camera configuration failed. The previous setting was restored when possible. Refresh and "
                           "check Diagnostics."));
        return;
    }
    if (!parseList(m_output))
    {
        fail(translate("Camera configuration returned an invalid response."));
        return;
    }
    if (m_mutation)
    {
        if ((m_expected.isEmpty() && m_state != State::Automatic) ||
            (!m_expected.isEmpty() && m_paths.value(m_selectedToken) != m_expected))
        {
            fail(translate("Camera setting changed during verification. Refresh and check Diagnostics."));
            return;
        }
        m_message = translate("Camera setting verified. The authentication service was restarted.");
    }
    else
    {
        m_message = m_state == State::Unknown
                        ? translate("The configured camera could not be verified.")
                        : translate("Camera configuration checked without starting a camera stream.");
    }
    Q_EMIT changed();
}
bool AuthCameraConfiguration::parseList(const QByteArray &output)
{
    const auto lines = output.split('\n');
    if (lines.size() < 3 || lines.first() != "KFCAMERA1" || !lines.last().isEmpty() || lines.size() > 131)
        return false;
    State state = State::Unknown;
    QByteArray selected;
    if (lines[1] == "automatic")
        state = State::Automatic;
    else if (lines[1] == "unknown")
        state = State::Unknown;
    else if (lines[1].startsWith("selected "))
    {
        selected = decode(lines[1].mid(9));
        if (!validDevicePath(selected))
            return false;
        state = State::Selected;
    }
    else
        return false;
    QHash<QString, QByteArray> paths;
    QVariantList devices;
    QSet<QByteArray> seen;
    QString selectedToken;
    for (qsizetype index = 2; index < lines.size() - 1; ++index)
    {
        const auto fields = lines[index].split(' ');
        if (fields.size() != 3 || fields.first() != "device")
            return false;
        const auto path = decode(fields[1]);
        const auto label = decode(fields[2]);
        if (!validDevicePath(path) || label.isEmpty() || label.size() > 128 || label.contains('\0') ||
            label.contains('\n') || label.contains('\r') || label.contains('/') || seen.contains(path))
            return false;
        seen.insert(path);
        const auto token = QUuid::createUuid().toString(QUuid::WithoutBraces);
        paths.insert(token, path);
        devices.append(
            QVariantMap{{QStringLiteral("label"), QString::fromUtf8(label)}, {QStringLiteral("token"), token}});
        if (path == selected)
            selectedToken = token;
    }
    if (state == State::Selected && selectedToken.isEmpty())
        state = State::Unknown;
    m_state = state;
    m_selectedToken = selectedToken;
    m_paths = paths;
    m_devices = devices;
    return true;
}
