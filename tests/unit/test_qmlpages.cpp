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
    void analysisCancelsWhenApplicationDeactivates();
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
    for (int index = 0; index < 4; ++index)
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
    QVERIFY(setup);
    QVERIFY(test);
    QVERIFY(diagnostics);

    for (QObject *page : {home.get(), setup.get(), test.get(), diagnostics.get()})
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
             setup->findChild<QObject *>(QStringLiteral("cameraDeviceSelector")),
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
        const QString accessibleText = control->property("text").isValid()
                                           ? control->property("text").toString()
                                           : control->property("accessibilityLabel").toString();
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
