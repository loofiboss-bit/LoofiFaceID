// SPDX-License-Identifier: GPL-3.0-or-later

#include "supportreport.h"

#include "camerapreviewsession.h"
#include "systemstate.h"

#include <QClipboard>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QGuiApplication>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#include <QSet>

#ifndef KFACEAUTH_SUPPORT_PREFIX
#define KFACEAUTH_SUPPORT_PREFIX "kfaceauth-support"
#endif

namespace
{
QString translate(const char *text)
{
    return QCoreApplication::translate("SupportReport", text);
}
} // namespace

SupportReport::SupportReport(SystemState *systemState, CameraPreviewSession *cameraPreviewSession, QObject *parent)
    : QObject(parent), m_systemState(systemState), m_cameraPreviewSession(cameraPreviewSession)
{
    Q_ASSERT(m_systemState);
    connect(m_systemState, &SystemState::stateChanged, this, &SupportReport::rebuild);
    if (m_cameraPreviewSession)
    {
        connect(m_cameraPreviewSession, &CameraPreviewSession::devicesChanged, this, &SupportReport::rebuild);
        connect(m_cameraPreviewSession, &CameraPreviewSession::stateChanged, this, &SupportReport::rebuild);
    }
    rebuild();
}

QString SupportReport::report() const
{
    return m_report;
}

QString SupportReport::issueCode() const
{
    return m_issueCode;
}

QString SupportReport::issueTitle() const
{
    return m_issueTitle;
}

QString SupportReport::recommendedAction() const
{
    return m_recommendedAction;
}

QString SupportReport::lastExportPath() const
{
    return m_lastExportPath;
}

QString SupportReport::statusText() const
{
    return m_statusText;
}

bool SupportReport::hasIssue() const
{
    return !m_issueCode.isEmpty();
}

void SupportReport::setTransientIssueCode(const QString &code)
{
    QString normalized = code;
    static const QSet<QString> walletCodes = {
        QStringLiteral("vault-key-unavailable"),
        QStringLiteral("vault-key-delete-failed"),
        QStringLiteral("vault-key-rollback-failed"),
    };
    static const QSet<QString> timeoutCodes = {
        QStringLiteral("startup-timeout"),
        QStringLiteral("inference-timeout"),
        QStringLiteral("shutdown-timeout"),
        QStringLiteral("identity-startup-timeout"),
        QStringLiteral("identity-operation-timeout"),
        QStringLiteral("identity-shutdown-timeout"),
    };
    static const QSet<QString> workerCodes = {
        QStringLiteral("startup-failed"),
        QStringLiteral("worker-crashed"),
        QStringLiteral("worker-busy"),
        QStringLiteral("identity-startup-failed"),
        QStringLiteral("identity-worker-failed"),
        QStringLiteral("identity-worker-busy"),
    };
    static const QSet<QString> protocolCodes = {
        QStringLiteral("protocol-error"),
        QStringLiteral("invalid-frame"),
        QStringLiteral("stale-response"),
        QStringLiteral("identity-protocol-error"),
    };
    if (walletCodes.contains(code))
        normalized = QStringLiteral("kwallet-unavailable");
    else if (code == QLatin1String("analysis-error-11"))
        normalized = QStringLiteral("model-unavailable");
    else if (code == QLatin1String("analysis-error-12"))
        normalized = QStringLiteral("worker-crashed");
    else if (code == QLatin1String("analysis-error-13"))
        normalized.clear();
    else if (code == QLatin1String("analysis-error-14"))
        normalized = QStringLiteral("invalid-runtime-output");
    else if (code == QLatin1String("analysis-error-15"))
        normalized = QStringLiteral("worker-crashed");
    else if (timeoutCodes.contains(code))
        normalized = QStringLiteral("worker-timeout");
    else if (workerCodes.contains(code))
        normalized = QStringLiteral("worker-crashed");
    else if (protocolCodes.contains(code))
        normalized = QStringLiteral("protocol-error");

    static const QSet<QString> allowedCodes = {
        QStringLiteral("model-unavailable"),
        QStringLiteral("model-mismatch"),
        QStringLiteral("vault-locked"),
        QStringLiteral("vault-unreadable"),
        QStringLiteral("vault-model-mismatch"),
        QStringLiteral("kwallet-locked"),
        QStringLiteral("kwallet-unavailable"),
        QStringLiteral("worker-crashed"),
        QStringLiteral("worker-timeout"),
        QStringLiteral("invalid-runtime-output"),
        QStringLiteral("identity-worker-unavailable"),
        QStringLiteral("identity-protocol-error"),
        QStringLiteral("protocol-error"),
        QStringLiteral("vault-unavailable"),
        QStringLiteral("unsupported-platform"),
    };
    m_transientIssueCode = allowedCodes.contains(normalized) ? normalized : QString();
    rebuild();
}

void SupportReport::copyReport()
{
    if (auto *clipboard = QGuiApplication::clipboard())
    {
        clipboard->setText(m_report, QClipboard::Clipboard);
        m_statusText = translate("The redacted support report was copied.");
    }
    else
    {
        m_statusText = translate("The text could not be copied.");
    }
    Q_EMIT exportChanged();
}

bool SupportReport::exportReport()
{
    QString directory = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (directory.isEmpty())
        directory = QStandardPaths::writableLocation(QStandardPaths::HomeLocation);
    return exportToDirectory(directory);
}

bool SupportReport::exportToDirectory(const QString &directory)
{
    if (directory.isEmpty() || !QDir().mkpath(directory))
    {
        m_statusText = translate("The support report could not be exported.");
        Q_EMIT exportChanged();
        return false;
    }

    const QString stamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
    const QString path = QDir(directory).filePath(QStringLiteral(KFACEAUTH_SUPPORT_PREFIX "-%1.md").arg(stamp));
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text) || file.write(m_report.toUtf8()) < 0 || !file.commit())
    {
        m_statusText = translate("The support report could not be exported.");
        Q_EMIT exportChanged();
        return false;
    }

    m_lastExportPath = path;
    m_statusText = translate("The redacted support report was exported to Documents.");
    Q_EMIT exportChanged();
    return true;
}

QString SupportReport::redactedValue(const QString &value)
{
    QString sanitized = value.left(128);
    sanitized.replace(QRegularExpression(QStringLiteral(R"([\r\n\x00-\x1f])")), QStringLiteral(" "));
    sanitized.replace(QRegularExpression(QStringLiteral(R"((?:/home|/dev|/etc|/run/user)/[^\s]*)")),
                      QStringLiteral("[redacted]"));
    sanitized.replace(QRegularExpression(QStringLiteral(R"([A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,})")),
                      QStringLiteral("[redacted]"));
    sanitized.replace(
        QRegularExpression(QStringLiteral(
            R"((?i)\b(?:password|passwd|token|secret|credential|embedding|template|frame|image)\b\s*[:=]\s*\S+)")),
        QStringLiteral("[redacted]"));
    sanitized.replace(QRegularExpression(QStringLiteral(R"(\b(?:sk|ghp|glpat)-[A-Za-z0-9_-]{8,}\b)")),
                      QStringLiteral("[redacted]"));
    return sanitized.trimmed().isEmpty() ? QStringLiteral("unknown") : sanitized.trimmed();
}

QString SupportReport::titleForCode(const QString &code)
{
    if (code == QLatin1String("camera-busy"))
        return translate("The camera is in use");
    if (code == QLatin1String("camera-unavailable"))
        return translate("The camera is unavailable");
    if (code == QLatin1String("unsupported-platform"))
        return translate("This system is not qualified");
    if (code == QLatin1String("native-engine-unavailable"))
        return translate("The native engine is unavailable");
    if (code == QLatin1String("native-protocol-unavailable"))
        return translate("The native protocol is unavailable");
    if (code == QLatin1String("model-unavailable"))
        return translate("The verified model inventory is unavailable");
    if (code == QLatin1String("model-mismatch") || code == QLatin1String("vault-model-mismatch"))
        return translate("The profile model does not match");
    if (code == QLatin1String("vault-locked") || code == QLatin1String("kwallet-locked"))
        return translate("KWallet needs attention");
    if (code == QLatin1String("vault-unreadable"))
        return translate("The encrypted profile is unreadable");
    if (code == QLatin1String("vault-unavailable"))
        return translate("Encrypted profile status is unavailable");
    if (code == QLatin1String("kwallet-unavailable"))
        return translate("KWallet is unavailable");
    if (code == QLatin1String("invalid-runtime-output"))
        return translate("The local vision result failed validation");
    if (code == QLatin1String("worker-crashed") || code == QLatin1String("worker-timeout") ||
        code == QLatin1String("identity-worker-unavailable") || code == QLatin1String("identity-protocol-error") ||
        code == QLatin1String("protocol-error"))
        return translate("A local worker needs attention");
    return code.isEmpty() ? QString() : translate("KFaceAuth needs attention");
}

QString SupportReport::actionForCode(const QString &code)
{
    if (code == QLatin1String("camera-busy"))
        return translate("Close applications using the camera, then retry the preview.");
    if (code == QLatin1String("camera-unavailable"))
        return translate("Reconnect or re-enable the camera, then refresh camera discovery.");
    if (code == QLatin1String("unsupported-platform"))
        return translate(
            "This build is qualified only for Fedora 44 with KDE Plasma. Detected values are shown in Diagnostics.");
    if (code == QLatin1String("native-engine-unavailable") || code == QLatin1String("native-protocol-unavailable"))
        return translate("Verify the installed local identity worker and model inventory. PAM and system "
                         "authentication remain unsupported.");
    if (code == QLatin1String("model-unavailable"))
        return translate(
            "Verify the installed YuNet and SFace files against the offline model manifest, then refresh.");
    if (code == QLatin1String("model-mismatch") || code == QLatin1String("vault-model-mismatch"))
        return translate(
            "Keep the profile unchanged and create a new profile only after confirming the verified model inventory.");
    if (code == QLatin1String("vault-unreadable"))
        return translate(
            "Preserve the unreadable profile until you explicitly confirm reset; reset requires enrollment again.");
    if (code == QLatin1String("vault-unavailable"))
        return translate("Retry Diagnostics once. Keep the existing profile unchanged until its status is readable.");
    if (code == QLatin1String("vault-locked") || code == QLatin1String("kwallet-locked"))
        return translate("Unlock KWallet in the current session, then retry. KFaceAuth has no plaintext-key fallback.");
    if (code == QLatin1String("kwallet-unavailable"))
        return translate(
            "Enable and unlock KWallet in the current user session, then refresh. No profile key is stored elsewhere.");
    if (code == QLatin1String("invalid-runtime-output"))
        return translate("Discard the result and retry. If invalid results continue, refresh Diagnostics; this error "
                         "does not indicate that OpenCV needs reinstalling.");
    if (code == QLatin1String("worker-crashed") || code == QLatin1String("worker-timeout") ||
        code == QLatin1String("identity-worker-unavailable") || code == QLatin1String("identity-protocol-error") ||
        code == QLatin1String("protocol-error"))
        return translate("Retry once. If the issue continues, export this bounded report; worker failures clear frames "
                         "and results.");
    if (code.isEmpty())
        return translate("No known issue is currently reported.");
    return translate("Keep unsupported operations disabled and export the redacted report for development support.");
}

void SupportReport::rebuild()
{
    m_issueCode = currentIssueCode();
    m_issueTitle = titleForCode(m_issueCode);
    m_recommendedAction = actionForCode(m_issueCode);

    m_report =
        QStringLiteral("# KFaceAuth support report\n\n"
                       "This report contains bounded local status only. It excludes device identifiers, images, "
                       "biometric data, paths, and credentials.\n\n"
                       "- Data source: %1\n"
                       "- Distribution: %2\n"
                       "- Fedora version: %3\n"
                       "- Plasma: %4\n"
                       "- Display manager: %5\n"
                       "- Native engine: %6\n"
                       "- Engine version: %7\n"
                       "- Vision: %8\n"
                       "- Enrollment: %9\n"
                       "- Authentication decisions: %10\n"
                       "- PAM configuration: %11\n"
                       "- Template persistence: %12\n"
                       "- Secure Boot: %13\n"
                       "- Diagnostic code: %14\n"
                       "- Native cameras: total=%15 rgb=%16 ir=%17 unknown=%18\n"
                       "- Native preview error: %19\n"
                       "- Native preview dropped frames: %20\n")
            .arg(redactedValue(m_systemState->dataSource()), redactedValue(m_systemState->distribution()),
                 redactedValue(m_systemState->fedoraVersion()), redactedValue(m_systemState->plasmaVersion()),
                 redactedValue(m_systemState->activeDisplayManager()),
                 redactedValue(m_systemState->engineStatusLabel()), redactedValue(m_systemState->engineVersion()),
                 redactedValue(m_systemState->visionStatusLabel()),
                 redactedValue(m_systemState->enrollmentStatusLabel()),
                 redactedValue(m_systemState->authenticationStatusLabel()),
                 redactedValue(m_systemState->pamStatusLabel()),
                 redactedValue(m_systemState->templatePersistenceStatusLabel()),
                 redactedValue(m_systemState->secureBootStatusLabel()),
                 redactedValue(m_issueCode.isEmpty() ? QStringLiteral("none") : m_issueCode))
            .arg(m_cameraPreviewSession ? m_cameraPreviewSession->deviceCount() : 0)
            .arg(m_cameraPreviewSession ? m_cameraPreviewSession->deviceCountForSpectrum(QStringLiteral("rgb")) : 0)
            .arg(m_cameraPreviewSession ? m_cameraPreviewSession->deviceCountForSpectrum(QStringLiteral("ir")) : 0)
            .arg(m_cameraPreviewSession ? m_cameraPreviewSession->deviceCountForSpectrum(QStringLiteral("unknown")) : 0)
            .arg(m_cameraPreviewSession && !m_cameraPreviewSession->errorCode().isEmpty()
                     ? m_cameraPreviewSession->errorCode()
                     : QStringLiteral("none"))
            .arg(m_cameraPreviewSession ? m_cameraPreviewSession->droppedFrames() : 0);
    Q_EMIT reportChanged();
}

QString SupportReport::currentIssueCode() const
{
    if (m_cameraPreviewSession && !m_cameraPreviewSession->errorCode().isEmpty())
        return m_cameraPreviewSession->errorCode();
    if (!m_transientIssueCode.isEmpty())
        return m_transientIssueCode;
    return m_systemState->issueCode();
}
