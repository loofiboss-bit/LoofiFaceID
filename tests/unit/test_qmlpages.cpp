// SPDX-License-Identifier: GPL-3.0-or-later

#include "camerapreviewitem.h"
#include "camerapreviewsession.h"
#include "enrollmentsession.h"
#include "identityworkerclient.h"
#include "kwalletkeyprovider.h"
#include "localverificationsession.h"
#include "supportreport.h"
#include "systemstate.h"
#include "visionanalysissession.h"

#include <KLocalizedQmlContext>

#include <QGuiApplication>
#include <QProcessEnvironment>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTest>
#include <algorithm>
#include <qqml.h>

#ifndef KFACEAUTH_VERSION_STRING
#error "KFACEAUTH_VERSION_STRING must be defined"
#endif

class QmlPagesTest final : public QObject
{
    Q_OBJECT

  private Q_SLOTS:
    void mainSurfaceCreatesAndNavigates();
    void mainSurfaceHandlesUnavailableBackend();
    void destinationPagesCreateForUnavailableEngine();
    void setupPageStopsWhenHidden();
    void setupRecoveryReplacementAndResponsiveFocus();
    void analysisCancelsWhenApplicationDeactivates();
    void authenticationModesAndReadbackFeedback();
};

class QmlKcmFacade final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(SystemState *systemState READ systemState CONSTANT)
    Q_PROPERTY(CameraPreviewSession *cameraPreviewSession READ cameraPreviewSession CONSTANT)
    Q_PROPERTY(VisionAnalysisSession *visionAnalysisSession READ visionAnalysisSession CONSTANT)
    Q_PROPERTY(EnrollmentSession *enrollmentSession READ enrollmentSession CONSTANT)
    Q_PROPERTY(LocalVerificationSession *localVerificationSession READ localVerificationSession CONSTANT)
    Q_PROPERTY(SupportReport *supportReport READ supportReport CONSTANT)
    Q_PROPERTY(bool refreshing READ refreshing CONSTANT)
    Q_PROPERTY(bool partialDiagnostics READ refreshing CONSTANT)
    Q_PROPERTY(bool retryAvailable READ refreshing CONSTANT)
    Q_PROPERTY(QString productVersion READ productVersion CONSTANT)
    Q_PROPERTY(QString flowStateLabel READ flowStateLabel CONSTANT)
    Q_PROPERTY(QString recommendedAction READ recommendedAction CONSTANT)
    Q_PROPERTY(bool needsCamera READ needsCamera CONSTANT)
    Q_PROPERTY(bool needsProfile READ needsProfile CONSTANT)
    Q_PROPERTY(bool readyToTest READ readyToTest CONSTANT)
    Q_PROPERTY(bool needsAttention READ needsAttention CONSTANT)

  public:
    QmlKcmFacade(SystemState *systemState, CameraPreviewSession *cameraPreviewSession,
                 VisionAnalysisSession *visionAnalysisSession, EnrollmentSession *enrollmentSession,
                 LocalVerificationSession *localVerificationSession, SupportReport *supportReport,
                 QObject *parent = nullptr)
        : QObject(parent), m_systemState(systemState), m_cameraPreviewSession(cameraPreviewSession),
          m_visionAnalysisSession(visionAnalysisSession), m_enrollmentSession(enrollmentSession),
          m_localVerificationSession(localVerificationSession), m_supportReport(supportReport)
    {
    }

    SystemState *systemState() const
    {
        return m_systemState;
    }
    CameraPreviewSession *cameraPreviewSession() const
    {
        return m_cameraPreviewSession;
    }
    VisionAnalysisSession *visionAnalysisSession() const
    {
        return m_visionAnalysisSession;
    }
    EnrollmentSession *enrollmentSession() const
    {
        return m_enrollmentSession;
    }
    LocalVerificationSession *localVerificationSession() const
    {
        return m_localVerificationSession;
    }
    SupportReport *supportReport() const
    {
        return m_supportReport;
    }
    bool refreshing() const
    {
        return false;
    }
    QString productVersion() const
    {
        return QStringLiteral(KFACEAUTH_VERSION_STRING);
    }
    QString flowStateLabel() const
    {
        return QStringLiteral("Camera setup needed");
    }
    QString recommendedAction() const
    {
        return QStringLiteral("Set up camera");
    }
    bool needsCamera() const
    {
        return true;
    }
    bool needsProfile() const
    {
        return true;
    }
    bool readyToTest() const
    {
        return false;
    }
    bool needsAttention() const
    {
        return false;
    }

    Q_INVOKABLE void refresh() {}

  private:
    SystemState *m_systemState = nullptr;
    CameraPreviewSession *m_cameraPreviewSession = nullptr;
    VisionAnalysisSession *m_visionAnalysisSession = nullptr;
    EnrollmentSession *m_enrollmentSession = nullptr;
    LocalVerificationSession *m_localVerificationSession = nullptr;
    SupportReport *m_supportReport = nullptr;
};

namespace
{
std::unique_ptr<QObject> createPage(QQmlEngine &engine, const QString &fileName, const QVariantMap &properties)
{
    const QString path = QStringLiteral(KFACEAUTH_SOURCE_DIR "/src/kcm/ui/") + fileName;
    QQmlComponent component(&engine, QUrl::fromLocalFile(path));
    if (component.isError())
    {
        qWarning().noquote() << component.errorString();
        return {};
    }

    auto object = std::unique_ptr<QObject>(component.createWithInitialProperties(properties));
    if (!object)
        qWarning().noquote() << component.errorString();
    return object;
}

QProcessEnvironment environmentFor(const QString &mode)
{
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("KFACEAUTH_FAKE_VISION_MODE"), mode);
    return environment;
}
} // namespace

void QmlPagesTest::mainSurfaceCreatesAndNavigates()
{
    SystemState state;
    CameraPreviewSession cameraPreviewSession(QStringLiteral("/nonexistent/preview-worker"), nullptr);
    VisionAnalysisSession visionAnalysisSession(&cameraPreviewSession, QStringLiteral("/nonexistent/vision-worker"),
                                                nullptr);
    KWalletKeyProvider keyProvider;
    IdentityWorkerClient identityWorker(QStringLiteral("/nonexistent/identity-worker"), {}, nullptr);
    EnrollmentSession enrollmentSession(&cameraPreviewSession, &identityWorker, &keyProvider);
    LocalVerificationSession localVerificationSession(&cameraPreviewSession, &identityWorker, &keyProvider);
    SupportReport supportReport(&state, &cameraPreviewSession);
    QmlKcmFacade facade(&state, &cameraPreviewSession, &visionAnalysisSession, &enrollmentSession,
                        &localVerificationSession, &supportReport);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("kcm"), &facade);
    auto *localizedContext = KLocalization::setupLocalizedContext(&engine);
    localizedContext->setTranslationDomain(QStringLiteral("kcm_kfaceauth"));

    QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(KFACEAUTH_SOURCE_DIR "/src/kcm/ui/main.qml")));
    QVERIFY2(!component.isError(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    auto *root = qobject_cast<QQuickItem *>(object.get());
    QVERIFY(root);

    const QList<QPair<QString, QString>> destinations = {
        {QStringLiteral("homeRefreshButton"), QStringLiteral("homeTab")},
        {QStringLiteral("cameraDeviceSelector"), QStringLiteral("setupTab")},
        {QStringLiteral("verifyButton"), QStringLiteral("testTab")},
        {QStringLiteral("sddmAuthMode"), QStringLiteral("authIntegrationTab")},
        {QStringLiteral("diagnosticsRefreshButton"), QStringLiteral("diagnosticsTab")},
    };
    auto *tabs = object->findChild<QObject *>(QStringLiteral("navigationTabs"));
    QVERIFY(tabs);
    for (int width : {320, 480, 960})
    {
        root->setSize(QSizeF(width, 720));
        QCoreApplication::processEvents();
        QVERIFY(root->implicitHeight() > 0);
        for (int index = 0; index < destinations.size(); ++index)
        {
            auto *tab = object->findChild<QObject *>(destinations.at(index).second);
            QVERIFY(tab);
            tabs->setProperty("currentIndex", index);
            QCoreApplication::processEvents();
            auto *control = object->findChild<QObject *>(destinations.at(index).first);
            QVERIFY(control);
            QVERIFY(control->property("activeFocusOnTab").toBool());
        }
    }
}

void QmlPagesTest::mainSurfaceHandlesUnavailableBackend()
{
    QmlKcmFacade facade(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    QQmlEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("kcm"), &facade);
    auto *localizedContext = KLocalization::setupLocalizedContext(&engine);
    localizedContext->setTranslationDomain(QStringLiteral("kcm_kfaceauth"));

    QList<QQmlError> warnings;
    connect(&engine, &QQmlEngine::warnings, &engine,
            [&warnings](const QList<QQmlError> &errors) { warnings.append(errors); });

    QQmlComponent component(&engine, QUrl::fromLocalFile(QStringLiteral(KFACEAUTH_SOURCE_DIR "/src/kcm/ui/main.qml")));
    QVERIFY2(!component.isError(), qPrintable(component.errorString()));
    std::unique_ptr<QObject> object(component.create());
    QVERIFY2(object, qPrintable(component.errorString()));
    QCOMPARE(object->property("backendReady").toBool(), false);

    auto *tabs = object->findChild<QObject *>(QStringLiteral("navigationTabs"));
    QVERIFY(tabs);
    for (int index = 0; index < 5; ++index)
    {
        tabs->setProperty("currentIndex", index);
        QCoreApplication::processEvents();
        const auto messages = object->findChildren<QObject *>(QStringLiteral("backendInitializationMessage"));
        QVERIFY(!messages.isEmpty());
        QVERIFY(std::any_of(messages.cbegin(), messages.cend(),
                            [](QObject *message) { return message->property("visible").toBool(); }));
    }

    for (const QQmlError &warning : warnings)
        QVERIFY2(!warning.toString().contains(QStringLiteral("TypeError")), qPrintable(warning.toString()));
}

void QmlPagesTest::destinationPagesCreateForUnavailableEngine()
{
    SystemStateSnapshot snapshot;
    snapshot.headline = QStringLiteral("Native engine unavailable");
    snapshot.summary = QStringLiteral("Camera checks remain available.");
    snapshot.issueCode = QStringLiteral("native-engine-unavailable");
    SystemState state;
    state.apply(snapshot);
    CameraPreviewSession cameraPreviewSession(QStringLiteral("/nonexistent/preview-worker"), nullptr);
    VisionAnalysisSession visionAnalysisSession(&cameraPreviewSession, QStringLiteral("/nonexistent/vision-worker"),
                                                nullptr);
    KWalletKeyProvider keyProvider;
    IdentityWorkerClient identityWorker(QStringLiteral("/nonexistent/identity-worker"), {}, nullptr);
    EnrollmentSession enrollmentSession(&cameraPreviewSession, &identityWorker, &keyProvider);
    LocalVerificationSession localVerificationSession(&cameraPreviewSession, &identityWorker, &keyProvider);
    SupportReport supportReport(&state, &cameraPreviewSession);
    QQmlEngine engine;
    auto *localizedContext = KLocalization::setupLocalizedContext(&engine);
    localizedContext->setTranslationDomain(QStringLiteral("kcm_kfaceauth"));

    auto home = createPage(engine, QStringLiteral("HomePage.qml"),
                           {
                               {QStringLiteral("systemState"), QVariant::fromValue(&state)},
                               {QStringLiteral("cameraPreviewSession"), QVariant::fromValue(&cameraPreviewSession)},
                               {QStringLiteral("enrollmentSession"), QVariant::fromValue(&enrollmentSession)},
                               {QStringLiteral("productVersion"), QStringLiteral(KFACEAUTH_VERSION_STRING)},
                               {QStringLiteral("flowStateLabel"), QStringLiteral("Camera setup needed")},
                               {QStringLiteral("recommendedAction"), QStringLiteral("Set up camera")},
                               {QStringLiteral("needsCamera"), true},
                               {QStringLiteral("needsProfile"), true},
                               {QStringLiteral("readyToTest"), false},
                               {QStringLiteral("needsAttention"), true},
                               {QStringLiteral("refreshActive"), false},
                           });
    auto authIntegration =
        createPage(engine, QStringLiteral("AuthIntegrationPage.qml"),
                   {
                       {QStringLiteral("backendReady"), true},
                       {QStringLiteral("enrollmentSession"), QVariant::fromValue(&enrollmentSession)},
                   });
    auto setup = createPage(engine, QStringLiteral("SetupPage.qml"),
                            {
                                {QStringLiteral("systemState"), QVariant::fromValue(&state)},
                                {QStringLiteral("cameraPreviewSession"), QVariant::fromValue(&cameraPreviewSession)},
                                {QStringLiteral("visionAnalysisSession"), QVariant::fromValue(&visionAnalysisSession)},
                                {QStringLiteral("enrollmentSession"), QVariant::fromValue(&enrollmentSession)},
                            });
    auto test =
        createPage(engine, QStringLiteral("TestPage.qml"),
                   {
                       {QStringLiteral("cameraPreviewSession"), QVariant::fromValue(&cameraPreviewSession)},
                       {QStringLiteral("localVerificationSession"), QVariant::fromValue(&localVerificationSession)},
                   });
    auto diagnostics =
        createPage(engine, QStringLiteral("DiagnosticsPage.qml"),
                   {
                       {QStringLiteral("systemState"), QVariant::fromValue(&state)},
                       {QStringLiteral("supportReport"), QVariant::fromValue(&supportReport)},
                       {QStringLiteral("cameraPreviewSession"), QVariant::fromValue(&cameraPreviewSession)},
                       {QStringLiteral("refreshActive"), false},
                   });
    QVERIFY(home);
    QVERIFY(authIntegration);
    QVERIFY(setup);
    QVERIFY(test);
    QVERIFY(diagnostics);

    for (QObject *page : {home.get(), setup.get(), test.get(), authIntegration.get(), diagnostics.get()})
    {
        auto *item = qobject_cast<QQuickItem *>(page);
        QVERIFY(item);
        for (const int width : {320, 480, 960})
        {
            item->setSize(QSizeF(width, 720));
            QCoreApplication::processEvents();
            QVERIFY(item->implicitHeight() > 0);
        }
    }

    for (QObject *control : {
             home->findChild<QObject *>(QStringLiteral("homeRefreshButton")),
             home->findChild<QObject *>(QStringLiteral("primaryStatusAction")),
             home->findChild<QObject *>(QStringLiteral("homeSetupButton")),
             home->findChild<QObject *>(QStringLiteral("homeAuthIntegrationButton")),
             setup->findChild<QObject *>(QStringLiteral("cameraDeviceSelector")),
             authIntegration->findChild<QObject *>(QStringLiteral("sddmAuthMode")),
             authIntegration->findChild<QObject *>(QStringLiteral("plasmaLockAuthMode")),
             setup->findChild<QObject *>(QStringLiteral("cameraRefreshButton")),
             setup->findChild<QObject *>(QStringLiteral("cameraPreviewAction")),
             setup->findChild<QObject *>(QStringLiteral("visionAnalyzeAction")),
             setup->findChild<QObject *>(QStringLiteral("refreshStatusButton")),
             setup->findChild<QObject *>(QStringLiteral("deleteProfileButton")),
             setup->findChild<QObject *>(QStringLiteral("resetProfileButton")),
             setup->findChild<QObject *>(QStringLiteral("startEnrollmentButton")),
             setup->findChild<QObject *>(QStringLiteral("captureButton")),
             setup->findChild<QObject *>(QStringLiteral("retrySampleButton")),
             setup->findChild<QObject *>(QStringLiteral("cancelEnrollmentButton")),
             setup->findChild<QObject *>(QStringLiteral("finishEnrollmentButton")),
             test->findChild<QObject *>(QStringLiteral("cameraDeviceSelector")),
             test->findChild<QObject *>(QStringLiteral("cameraRefreshButton")),
             test->findChild<QObject *>(QStringLiteral("cameraPreviewAction")),
             test->findChild<QObject *>(QStringLiteral("verifyButton")),
             test->findChild<QObject *>(QStringLiteral("clearVerificationButton")),
             diagnostics->findChild<QObject *>(QStringLiteral("diagnosticsRefreshButton")),
             diagnostics->findChild<QObject *>(QStringLiteral("copyReportButton")),
             diagnostics->findChild<QObject *>(QStringLiteral("exportReportButton")),
         })
    {
        QVERIFY(control);
        QVERIFY(control->property("activeFocusOnTab").toBool());
        const QString accessibleText =
            control->property("text").isValid()                 ? control->property("text").toString()
            : control->property("accessibilityLabel").isValid() ? control->property("accessibilityLabel").toString()
            : control->property("displayText").isValid()        ? control->property("displayText").toString()
                                                                : control->property("currentText").toString();
        QVERIFY(!accessibleText.isEmpty());
    }

    for (QObject *dialog : {
             setup->findChild<QObject *>(QStringLiteral("deleteProfileConfirmation")),
             setup->findChild<QObject *>(QStringLiteral("resetProfileConfirmation")),
         })
    {
        QVERIFY(dialog);
        QVERIFY(dialog->property("modal").toBool());
        QVERIFY(!dialog->property("title").toString().isEmpty());
    }
}

namespace
{
class QmlTestKeyProvider final : public KWalletKeyProvider
{
  public:
    void requestKey(Completion completion) override
    {
        completion(Result{State::Available, QByteArray(32, char(0x41))});
    }
};
} // namespace

void QmlPagesTest::authenticationModesAndReadbackFeedback()
{
    EnrollmentSession::AuthStatusSnapshot snapshot;
    snapshot.components = snapshot.sddmApi = snapshot.sddmTheme = snapshot.sddmPam = snapshot.plasmaApi =
        snapshot.plasmaPam = snapshot.daemon = snapshot.systemProfile = EnrollmentSession::Readiness::Available;
    snapshot.sddmMode = QStringLiteral("manual");
    snapshot.plasmaMode = QStringLiteral("on-activity");
    CameraPreviewSession preview(QStringLiteral("/nonexistent/preview-worker"), nullptr);
    QmlTestKeyProvider keys;
    QProcessEnvironment identityEnvironment = QProcessEnvironment::systemEnvironment();
    identityEnvironment.insert(QStringLiteral("KFACEAUTH_TEST_MODE"), QStringLiteral("session-lifecycle"));
    IdentityWorkerClient worker(QStringLiteral(KFACEAUTH_FAKE_IDENTITY_WORKER_PATH), identityEnvironment, nullptr);
    EnrollmentSession::AuthOperationCompletion finishOperation;
    int launches = 0;
    EnrollmentSession enrollment(
        &preview, &worker, &keys,
        [&snapshot](EnrollmentSession::AuthStatusCompletion completion) { completion(snapshot); },
        [&finishOperation, &launches](QStringList, QByteArray input,
                                      EnrollmentSession::AuthOperationCompletion completion)
        {
            input.fill('\0');
            ++launches;
            finishOperation = std::move(completion);
        });
    enrollment.refreshProfileStatus();
    QTRY_VERIFY(enrollment.profileReady());
    QQmlEngine engine;
    KLocalization::setupLocalizedContext(&engine)->setTranslationDomain(QStringLiteral("kcm_kfaceauth"));
    auto page = createPage(engine, QStringLiteral("AuthIntegrationPage.qml"),
                           {{QStringLiteral("backendReady"), true},
                            {QStringLiteral("enrollmentSession"), QVariant::fromValue(&enrollment)}});
    QVERIFY(page);
    auto *item = qobject_cast<QQuickItem *>(page.get());
    QVERIFY(item);
    QQuickWindow window;
    item->setParentItem(window.contentItem());
    for (int width : {320, 480, 960})
    {
        window.resize(width, 720);
        item->setSize(QSizeF(width, 720));
        QCoreApplication::processEvents();
        for (const char *name :
             {"refreshAuthStatus", "openAuthRegistration", "syncSystemProfile", "sddmAuthMode", "plasmaLockAuthMode"})
        {
            auto *control = page->findChild<QQuickItem *>(QString::fromLatin1(name));
            QVERIFY(control);
            QVERIFY(control->property("activeFocusOnTab").toBool());
            QVERIFY(control->width() <= width);
        }
    }
    auto *mode = page->findChild<QObject *>(QStringLiteral("sddmAuthMode"));
    auto *feedback = page->findChild<QObject *>(QStringLiteral("authOperationFeedback"));
    QVERIFY(mode && feedback);
    QCOMPARE(mode->property("count").toInt(), 3);
    QCOMPARE(mode->property("currentIndex").toInt(), 2);
    mode->setProperty("currentIndex", 1);
    QVERIFY(QMetaObject::invokeMethod(mode, "activated", Q_ARG(int, 1)));
    QCOMPARE(launches, 1);
    QVERIFY(!mode->property("enabled").toBool());
    QCOMPARE(mode->property("currentIndex").toInt(), 2);
    QVERIFY(!feedback->property("text").toString().isEmpty());
    finishOperation(126, true);
    QCOMPARE(mode->property("currentIndex").toInt(), 2);
    QVERIFY(mode->property("enabled").toBool());
    const QString failedText = feedback->property("text").toString();
    QVERIFY(!failedText.isEmpty());
    enrollment.checkSystemAuthStatus();
    QCOMPARE(feedback->property("text").toString(), failedText);
    snapshot.sddmMode = QStringLiteral("unknown");
    enrollment.checkSystemAuthStatus();
    QCOMPARE(mode->property("currentIndex").toInt(), -1);
    QCOMPARE(mode->property("count").toInt(), 3);
    QVERIFY(!preview.previewActive());
}

void QmlPagesTest::setupRecoveryReplacementAndResponsiveFocus()
{
    CameraPreviewSession preview(QStringLiteral(KFACEAUTH_FAKE_PREVIEW_WORKER_PATH), nullptr);
    VisionAnalysisSession analysis(&preview, QStringLiteral(KFACEAUTH_FAKE_VISION_WORKER_PATH),
                                   environmentFor(QStringLiteral("crash")), nullptr);
    QmlTestKeyProvider keys;
    QProcessEnvironment identityEnvironment = QProcessEnvironment::systemEnvironment();
    identityEnvironment.insert(QStringLiteral("KFACEAUTH_TEST_MODE"), QStringLiteral("session-fail-commit"));
    IdentityWorkerClient worker(QStringLiteral(KFACEAUTH_FAKE_IDENTITY_WORKER_PATH), identityEnvironment, nullptr);
    SystemState state;
    EnrollmentSession enrollment(&preview, &worker, &keys);
    QQmlEngine engine;
    auto *localizedContext = KLocalization::setupLocalizedContext(&engine);
    localizedContext->setTranslationDomain(QStringLiteral("kcm_kfaceauth"));
    auto page = createPage(engine, QStringLiteral("SetupPage.qml"),
                           {{QStringLiteral("systemState"), QVariant::fromValue(&state)},
                            {QStringLiteral("cameraPreviewSession"), QVariant::fromValue(&preview)},
                            {QStringLiteral("visionAnalysisSession"), QVariant::fromValue(&analysis)},
                            {QStringLiteral("enrollmentSession"), QVariant::fromValue(&enrollment)},
                            {QStringLiteral("autoCaptureEnabled"), false}});
    QVERIFY(page);
    auto *item = qobject_cast<QQuickItem *>(page.get());
    QVERIFY(item);
    QQuickWindow window;
    window.resize(320, 720);
    item->setParentItem(window.contentItem());
    item->setSize(QSizeF(320, 720));
    window.show();
    window.requestActivate();
    QCoreApplication::processEvents();
    preview.refreshDevices();
    QTRY_COMPARE(preview.state(), CameraPreviewSession::State::Ready);
    preview.startPreview();
    QTRY_COMPARE(preview.state(), CameraPreviewSession::State::Streaming);
    QTRY_VERIFY(preview.frameAvailable());
    enrollment.setPageActive(true);
    QTRY_COMPARE(enrollment.profileState(), EnrollmentSession::ProfileState::Ready);
    enrollment.startEnrollment();
    QTRY_COMPARE(enrollment.state(), EnrollmentSession::State::Enrolling);
    for (int sample = 0; sample < 3; ++sample)
    {
        enrollment.captureSample(false);
        QTRY_COMPARE(enrollment.sampleCount(), sample + 1);
    }

    analysis.startGuidance();
    QTRY_COMPARE(analysis.state(), VisionAnalysisSession::State::Failed);
    QCOMPARE(enrollment.sampleCount(), 3);
    auto *guidanceRetry = page->findChild<QObject *>(QStringLiteral("retryGuidanceButton"));
    QVERIFY(guidanceRetry);
    QTRY_VERIFY(guidanceRetry->property("visible").toBool());
    QVERIFY(guidanceRetry->property("enabled").toBool());
    page->setProperty("stableObservations", 2);
    page->setProperty("lastGuidanceGeneration", QStringLiteral("stale"));
    QVERIFY(QMetaObject::invokeMethod(guidanceRetry, "clicked"));
    QCOMPARE(page->property("stableObservations").toInt(), 0);
    QCOMPARE(page->property("lastGuidanceGeneration").toString(), QString());
    QCOMPARE(enrollment.sampleCount(), 3);
    QVERIFY(analysis.continuousTracking());
    const int budget = preview.remainingSeconds();
    QVERIFY(QMetaObject::invokeMethod(page.get(), "retryGuidance"));
    QVERIFY(preview.remainingSeconds() <= budget);
    QCOMPARE(enrollment.sampleCount(), 3);
    analysis.stopGuidance();

    auto *guidance = page->findChild<QQuickItem *>(QStringLiteral("enrollmentGuidanceText"));
    auto *banner = page->findChild<QQuickItem *>(QStringLiteral("enrollmentGuidanceBanner"));
    QVERIFY(guidance);
    QVERIFY(banner);
    for (const int width : {320, 480, 960})
    {
        window.resize(width, 720);
        item->setSize(QSizeF(width, 720));
        QCoreApplication::processEvents();
        guidance->setProperty("text", QStringLiteral("En lång svensk instruktion som måste vara fullt läsbar "
                                                     "även i ett smalt fönster utan att texten klipps bort."));
        QTest::qWait(30);
        QVERIFY2(guidance->height() + 1 >= guidance->property("implicitHeight").toReal(),
                 qPrintable(QStringLiteral("width=%1 label=%2 implicit=%3 banner=%4")
                                .arg(width)
                                .arg(guidance->height())
                                .arg(guidance->property("implicitHeight").toReal())
                                .arg(banner->height())));
        QVERIFY(banner->height() >= guidance->height());
        QList<QQuickItem *> steps;
        const auto collectSteps = [&steps](auto &&self, QQuickItem *parent) -> void
        {
            for (auto *child : parent->childItems())
            {
                if (child->objectName() == QLatin1String("enrollmentPoseStep"))
                    steps.append(child);
                self(self, child);
            }
        };
        collectSteps(collectSteps, item);
        QCOMPARE(steps.size(), 5);
        for (auto *step : steps)
        {
            QVERIFY(step->width() > 0);
            QVERIFY(step->x() >= 0);
            QVERIFY(step->x() + step->width() <= step->parentItem()->width() + 1);
        }
    }

    auto *capture = page->findChild<QQuickItem *>(QStringLiteral("captureButton"));
    auto *retry = page->findChild<QQuickItem *>(QStringLiteral("retrySampleButton"));
    QVERIFY(capture);
    QVERIFY(retry);
    capture->forceActiveFocus();
    QTRY_VERIFY(capture->hasActiveFocus());
    QTest::keyClick(&window, Qt::Key_Tab);
    QTRY_VERIFY(retry->hasActiveFocus());
    QTest::keyClick(&window, Qt::Key_Tab, Qt::ShiftModifier);
    QTRY_VERIFY(capture->hasActiveFocus());

    QVERIFY(QMetaObject::invokeMethod(page.get(), "saveProfile"));
    auto *dialog = page->findChild<QObject *>(QStringLiteral("replaceProfileConfirmation"));
    QVERIFY(dialog);
    QTRY_VERIFY(dialog->property("visible").toBool());
    QCOMPARE(enrollment.state(), EnrollmentSession::State::ReadyToSave);
    QVERIFY(QMetaObject::invokeMethod(dialog, "reject"));
    QCOMPARE(enrollment.sampleCount(), 3);
    QVERIFY(enrollment.profileReady());
    QVERIFY(QMetaObject::invokeMethod(page.get(), "saveProfile"));
    QTRY_VERIFY(dialog->property("visible").toBool());
    QVERIFY(QMetaObject::invokeMethod(dialog, "accept"));
    QTRY_COMPARE(enrollment.state(), EnrollmentSession::State::Failed);
    QVERIFY(enrollment.profileReady());
    QCOMPARE(enrollment.storedSampleCount(), 5);
    item->setVisible(false);
    QTRY_COMPARE(preview.state(), CameraPreviewSession::State::Ready);
    item->setParentItem(nullptr);
}

void QmlPagesTest::setupPageStopsWhenHidden()
{
    CameraPreviewSession session(QStringLiteral(KFACEAUTH_FAKE_PREVIEW_WORKER_PATH), nullptr);
    VisionAnalysisSession analysis(&session, QStringLiteral(KFACEAUTH_FAKE_VISION_WORKER_PATH),
                                   environmentFor(QStringLiteral("timeout")), nullptr);
    KWalletKeyProvider keyProvider;
    IdentityWorkerClient identityWorker(QStringLiteral("/nonexistent/identity-worker"), {}, nullptr);
    SystemState state;
    EnrollmentSession enrollment(&session, &identityWorker, &keyProvider);
    QQmlEngine engine;
    auto *localizedContext = KLocalization::setupLocalizedContext(&engine);
    localizedContext->setTranslationDomain(QStringLiteral("kcm_kfaceauth"));
    auto page = createPage(engine, QStringLiteral("SetupPage.qml"),
                           {
                               {QStringLiteral("systemState"), QVariant::fromValue(&state)},
                               {QStringLiteral("cameraPreviewSession"), QVariant::fromValue(&session)},
                               {QStringLiteral("visionAnalysisSession"), QVariant::fromValue(&analysis)},
                               {QStringLiteral("enrollmentSession"), QVariant::fromValue(&enrollment)},
                           });
    QVERIFY(page);
    auto *item = qobject_cast<QQuickItem *>(page.get());
    QVERIFY(item);
    session.refreshDevices();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Ready);

    auto *previewAction = page->findChild<QObject *>(QStringLiteral("cameraPreviewAction"));
    auto *analyzeAction = page->findChild<QObject *>(QStringLiteral("visionAnalyzeAction"));
    QVERIFY(previewAction);
    QVERIFY(analyzeAction);
    QVERIFY(previewAction->property("activeFocusOnTab").toBool());
    QVERIFY(analyzeAction->property("activeFocusOnTab").toBool());

    session.startPreview();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Streaming);
    QTRY_VERIFY(session.frameAvailable());
    QTRY_VERIFY(analyzeAction->property("enabled").toBool());
    analysis.analyzeCurrentFrame();
    QTRY_COMPARE(analysis.state(), VisionAnalysisSession::State::Analyzing);
    item->setVisible(false);
    QTRY_COMPARE(analysis.state(), VisionAnalysisSession::State::Idle);
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Ready);
    QVERIFY(!session.frameAvailable());
    QVERIFY(!analysis.resultAvailable());
}

void QmlPagesTest::analysisCancelsWhenApplicationDeactivates()
{
    CameraPreviewSession session(QStringLiteral(KFACEAUTH_FAKE_PREVIEW_WORKER_PATH), nullptr);
    QProcessEnvironment environment = environmentFor(QStringLiteral("timeout"));
    VisionAnalysisSession analysis(&session, QStringLiteral(KFACEAUTH_FAKE_VISION_WORKER_PATH), environment, nullptr);
    session.refreshDevices();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Ready);
    session.startPreview();
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Streaming);
    QTRY_VERIFY(session.frameAvailable());
    analysis.analyzeCurrentFrame();
    QTRY_COMPARE(analysis.state(), VisionAnalysisSession::State::Analyzing);

    QMetaObject::invokeMethod(qGuiApp, "applicationStateChanged", Qt::DirectConnection,
                              Q_ARG(Qt::ApplicationState, Qt::ApplicationInactive));
    QTRY_COMPARE(analysis.state(), VisionAnalysisSession::State::Idle);
    QTRY_COMPARE(session.state(), CameraPreviewSession::State::Ready);
    QVERIFY(!analysis.resultAvailable());
}

int main(int argc, char **argv)
{
    QGuiApplication application(argc, argv);
    qmlRegisterType<CameraPreviewItem>(KFACEAUTH_QML_URI, 4, 0, "CameraPreview");
    qmlRegisterUncreatableType<CameraPreviewSession>(KFACEAUTH_QML_URI, 4, 0, "CameraPreviewSession",
                                                     QStringLiteral("provided by test"));
    qmlRegisterUncreatableType<VisionAnalysisSession>(KFACEAUTH_QML_URI, 4, 0, "VisionAnalysisSession",
                                                      QStringLiteral("provided by test"));
    qmlRegisterUncreatableType<EnrollmentSession>(KFACEAUTH_QML_URI, 4, 0, "EnrollmentSession",
                                                  QStringLiteral("provided by test"));
    qmlRegisterUncreatableType<LocalVerificationSession>(KFACEAUTH_QML_URI, 4, 0, "LocalVerificationSession",
                                                         QStringLiteral("provided by test"));
    QmlPagesTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "test_qmlpages.moc"
