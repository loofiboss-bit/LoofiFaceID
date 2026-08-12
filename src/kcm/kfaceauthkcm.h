// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "camerapreviewsession.h"
#include "enrollmentsession.h"
#include "faceauthbackend.h"
#include "identityworkerclient.h"
#include "kwalletkeyprovider.h"
#include "localverificationsession.h"
#include "refreshcoordinator.h"
#include "supportreport.h"
#include "systemprobe.h"
#include "systemstate.h"
#include "visionanalysissession.h"

#include <KQuickConfigModule>

#include <memory>

class KFaceAuthKcm final : public KQuickConfigModule
{
    Q_OBJECT

    Q_PROPERTY(SystemState *systemState READ systemState CONSTANT)
    Q_PROPERTY(CameraPreviewSession *cameraPreviewSession READ cameraPreviewSession CONSTANT)
    Q_PROPERTY(VisionAnalysisSession *visionAnalysisSession READ visionAnalysisSession CONSTANT)
    Q_PROPERTY(EnrollmentSession *enrollmentSession READ enrollmentSession CONSTANT)
    Q_PROPERTY(LocalVerificationSession *localVerificationSession READ localVerificationSession CONSTANT)
    Q_PROPERTY(SupportReport *supportReport READ supportReport CONSTANT)
    Q_PROPERTY(bool refreshing READ refreshing NOTIFY refreshStateChanged)
    Q_PROPERTY(bool partialDiagnostics READ partialDiagnostics NOTIFY refreshStateChanged)
    Q_PROPERTY(bool retryAvailable READ retryAvailable NOTIFY refreshStateChanged)
    Q_PROPERTY(QString productVersion READ productVersion CONSTANT)
    Q_PROPERTY(UiFlowState flowState READ flowState NOTIFY flowStateChanged)
    Q_PROPERTY(bool needsCamera READ needsCamera NOTIFY flowStateChanged)
    Q_PROPERTY(bool needsProfile READ needsProfile NOTIFY flowStateChanged)
    Q_PROPERTY(bool readyToTest READ readyToTest NOTIFY flowStateChanged)
    Q_PROPERTY(bool needsAttention READ needsAttention NOTIFY flowStateChanged)
    Q_PROPERTY(QString flowStateLabel READ flowStateLabel NOTIFY flowStateChanged)
    Q_PROPERTY(QString recommendedAction READ recommendedAction NOTIFY flowStateChanged)
    Q_PROPERTY(QString recommendedDestination READ recommendedDestination NOTIFY flowStateChanged)

  public:
    enum class UiFlowState
    {
        NeedsCamera,
        NeedsProfile,
        ReadyToTest,
        NeedsAttention,
    };
    Q_ENUM(UiFlowState)

    KFaceAuthKcm(QObject *parent, const KPluginMetaData &data);
    KFaceAuthKcm(QObject *parent, const KPluginMetaData &data, std::unique_ptr<FaceAuthBackend> backend);
    ~KFaceAuthKcm() override;

    [[nodiscard]] SystemState *systemState();
    [[nodiscard]] CameraPreviewSession *cameraPreviewSession();
    [[nodiscard]] VisionAnalysisSession *visionAnalysisSession();
    [[nodiscard]] EnrollmentSession *enrollmentSession();
    [[nodiscard]] LocalVerificationSession *localVerificationSession();
    [[nodiscard]] SupportReport *supportReport();
    [[nodiscard]] bool refreshing() const;
    [[nodiscard]] bool partialDiagnostics() const;
    [[nodiscard]] bool retryAvailable() const;
    [[nodiscard]] QString productVersion() const;
    [[nodiscard]] UiFlowState flowState() const;
    [[nodiscard]] bool needsCamera() const;
    [[nodiscard]] bool needsProfile() const;
    [[nodiscard]] bool readyToTest() const;
    [[nodiscard]] bool needsAttention() const;
    [[nodiscard]] QString flowStateLabel() const;
    [[nodiscard]] QString recommendedAction() const;
    [[nodiscard]] QString recommendedDestination() const;
    Q_INVOKABLE void refresh();

  Q_SIGNALS:
    void refreshStateChanged();
    void flowStateChanged();

  private:
    SystemProbe m_probe;
    SystemState m_systemState;
    CameraPreviewSession m_cameraPreviewSession;
    VisionAnalysisSession m_visionAnalysisSession;
    KWalletKeyProvider m_keyProvider;
    IdentityWorkerClient m_identityWorker;
    EnrollmentSession m_enrollmentSession;
    LocalVerificationSession m_localVerificationSession;
    SupportReport m_supportReport;
    RefreshCoordinator m_refreshCoordinator;
    quint64 m_probeGeneration = 0;
};
