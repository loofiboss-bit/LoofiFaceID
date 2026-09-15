// SPDX-License-Identifier: GPL-3.0-or-later

#include "kfaceauthkcm.h"

#include "camerapreviewitem.h"
#include "nativefaceauthbackend.h"

#include <KPluginFactory>
#include <QCoreApplication>
#include <qqml.h>

namespace
{
QString translate(const char *text)
{
    return QCoreApplication::translate("KFaceAuthKcm", text);
}
} // namespace

KFaceAuthKcm::KFaceAuthKcm(QObject *parent, const KPluginMetaData &data)
    : KFaceAuthKcm(parent, data, std::make_unique<NativeFaceAuthBackend>())
{
}

KFaceAuthKcm::KFaceAuthKcm(QObject *parent, const KPluginMetaData &data, std::unique_ptr<FaceAuthBackend> backend)
    : KQuickConfigModule(parent, data), m_probe(this), m_systemState(this), m_cameraPreviewSession(this),
      m_visionAnalysisSession(&m_cameraPreviewSession, this), m_keyProvider(this), m_identityWorker(this),
      m_enrollmentSession(&m_cameraPreviewSession, &m_identityWorker, &m_keyProvider, this),
      m_localVerificationSession(&m_cameraPreviewSession, &m_identityWorker, &m_keyProvider, this),
      m_supportReport(&m_systemState, &m_cameraPreviewSession, this), m_refreshCoordinator(std::move(backend), this)
{
    qmlRegisterType<CameraPreviewItem>(KFACEAUTH_QML_URI, 4, 0, "CameraPreview");
    qmlRegisterUncreatableType<CameraPreviewSession>(KFACEAUTH_QML_URI, 4, 0, "CameraPreviewSession",
                                                     QStringLiteral("CameraPreviewSession is provided by the KCM"));
    qmlRegisterUncreatableType<VisionAnalysisSession>(KFACEAUTH_QML_URI, 4, 0, "VisionAnalysisSession",
                                                      QStringLiteral("VisionAnalysisSession is provided by the KCM"));
    qmlRegisterUncreatableType<EnrollmentSession>(KFACEAUTH_QML_URI, 4, 0, "EnrollmentSession",
                                                  QStringLiteral("EnrollmentSession is provided by the KCM"));
    qmlRegisterUncreatableType<LocalVerificationSession>(
        KFACEAUTH_QML_URI, 4, 0, "LocalVerificationSession",
        QStringLiteral("LocalVerificationSession is provided by the KCM"));
    qmlRegisterUncreatableType<SystemState>(KFACEAUTH_QML_URI, 4, 0, "SystemState",
                                            QStringLiteral("SystemState is provided by the KCM"));
    qmlRegisterUncreatableType<SupportReport>(KFACEAUTH_QML_URI, 4, 0, "SupportReport",
                                              QStringLiteral("SupportReport is provided by the KCM"));
    setButtons(NoAdditionalButton);
    connect(&m_refreshCoordinator, &RefreshCoordinator::snapshotChanged, this,
            [this](const EngineSnapshot &snapshot)
            {
                const auto inProgress = [](const auto &result)
                { return result.state == ResultState::Pending || result.state == ResultState::Loading; };
                if (!inProgress(snapshot.protocol) && !inProgress(snapshot.status))
                    m_probe.requestProbe(++m_probeGeneration, snapshot);
            });
    connect(&m_probe, &SystemProbe::probeCompleted, this,
            [this](quint64 generation, const SystemStateSnapshot &snapshot)
            {
                if (generation == m_probeGeneration)
                    m_systemState.apply(snapshot);
            });
    connect(&m_refreshCoordinator, &RefreshCoordinator::stateChanged, this, &KFaceAuthKcm::refreshStateChanged);
    connect(&m_refreshCoordinator, &RefreshCoordinator::stateChanged, this, &KFaceAuthKcm::flowStateChanged);
    connect(&m_systemState, &SystemState::stateChanged, this, &KFaceAuthKcm::flowStateChanged);
    connect(&m_cameraPreviewSession, &CameraPreviewSession::stateChanged, this, &KFaceAuthKcm::flowStateChanged);
    connect(&m_cameraPreviewSession, &CameraPreviewSession::devicesChanged, this, &KFaceAuthKcm::flowStateChanged);
    connect(&m_cameraPreviewSession, &CameraPreviewSession::selectionChanged, this, &KFaceAuthKcm::flowStateChanged);
    connect(&m_enrollmentSession, &EnrollmentSession::stateChanged, this, &KFaceAuthKcm::flowStateChanged);
    connect(&m_enrollmentSession, &EnrollmentSession::profileChanged, this, &KFaceAuthKcm::flowStateChanged);
    const auto refreshTransientIssue = [this]()
    {
        QString code;
        if (!m_visionAnalysisSession.errorCode().isEmpty())
            code = m_visionAnalysisSession.errorCode();
        else if (!m_enrollmentSession.errorCode().isEmpty())
            code = m_enrollmentSession.errorCode();
        else if (!m_localVerificationSession.errorCode().isEmpty())
            code = m_localVerificationSession.errorCode();
        m_supportReport.setTransientIssueCode(code);
    };
    connect(&m_visionAnalysisSession, &VisionAnalysisSession::stateChanged, this, refreshTransientIssue);
    connect(&m_enrollmentSession, &EnrollmentSession::stateChanged, this, refreshTransientIssue);
    connect(&m_localVerificationSession, &LocalVerificationSession::stateChanged, this, refreshTransientIssue);
    refresh();
}

KFaceAuthKcm::~KFaceAuthKcm() = default;

SystemState *KFaceAuthKcm::systemState()
{
    return &m_systemState;
}

CameraPreviewSession *KFaceAuthKcm::cameraPreviewSession()
{
    return &m_cameraPreviewSession;
}

VisionAnalysisSession *KFaceAuthKcm::visionAnalysisSession()
{
    return &m_visionAnalysisSession;
}

EnrollmentSession *KFaceAuthKcm::enrollmentSession()
{
    return &m_enrollmentSession;
}

LocalVerificationSession *KFaceAuthKcm::localVerificationSession()
{
    return &m_localVerificationSession;
}

SupportReport *KFaceAuthKcm::supportReport()
{
    return &m_supportReport;
}

bool KFaceAuthKcm::refreshing() const
{
    return m_refreshCoordinator.refreshing();
}

bool KFaceAuthKcm::partialDiagnostics() const
{
    return m_refreshCoordinator.partialDiagnostics();
}

bool KFaceAuthKcm::retryAvailable() const
{
    return m_refreshCoordinator.retryAvailable();
}

QString KFaceAuthKcm::productVersion() const
{
    return QStringLiteral(KFACEAUTH_VERSION_STRING);
}

KFaceAuthKcm::UiFlowState KFaceAuthKcm::flowState() const
{
    if (needsAttention())
        return UiFlowState::NeedsAttention;
    if (needsCamera())
        return UiFlowState::NeedsCamera;
    if (needsProfile())
        return UiFlowState::NeedsProfile;
    return UiFlowState::ReadyToTest;
}

bool KFaceAuthKcm::needsCamera() const
{
    return !m_cameraPreviewSession.hasUsableCamera();
}

bool KFaceAuthKcm::needsProfile() const
{
    return !m_enrollmentSession.profileReady();
}

bool KFaceAuthKcm::readyToTest() const
{
    return !needsAttention() && !needsCamera() && !needsProfile();
}

bool KFaceAuthKcm::needsAttention() const
{
    return !m_systemState.issueCode().isEmpty() || !m_cameraPreviewSession.errorCode().isEmpty() ||
           m_enrollmentSession.profileNeedsAttention();
}

QString KFaceAuthKcm::flowStateLabel() const
{
    switch (flowState())
    {
    case UiFlowState::NeedsAttention:
        return translate("Resolve one issue");
    case UiFlowState::NeedsCamera:
        return translate("Camera setup needed");
    case UiFlowState::NeedsProfile:
        return translate("Face profile setup needed");
    case UiFlowState::ReadyToTest:
        return translate("Ready to test one frame");
    }
    return translate("Camera setup needed");
}

QString KFaceAuthKcm::recommendedAction() const
{
    switch (flowState())
    {
    case UiFlowState::NeedsAttention:
        return translate("Resolve issue");
    case UiFlowState::NeedsCamera:
        return translate("Set up camera");
    case UiFlowState::NeedsProfile:
        return translate("Create face profile");
    case UiFlowState::ReadyToTest:
        return translate("Test recognition");
    }
    return translate("Set up camera");
}

QString KFaceAuthKcm::recommendedDestination() const
{
    return needsAttention() ? QStringLiteral("diagnostics")
                            : (readyToTest() ? QStringLiteral("test") : QStringLiteral("setup"));
}

void KFaceAuthKcm::refresh()
{
    m_refreshCoordinator.requestRefresh();
}

K_PLUGIN_CLASS_WITH_JSON(KFaceAuthKcm, KFACEAUTH_PLUGIN_METADATA)

#include "kfaceauthkcm.moc"
