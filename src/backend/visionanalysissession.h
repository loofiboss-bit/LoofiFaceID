// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointF>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRectF>
#include <QString>
#include <QTimer>
#include <QVector>
#include <optional>

class CameraPreviewSession;
class VisionAnalysisSession final : public QObject
{
    Q_OBJECT

    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(bool canAnalyze READ canAnalyze NOTIFY availabilityChanged)
    Q_PROPERTY(bool resultAvailable READ resultAvailable NOTIFY resultChanged)
    Q_PROPERTY(int frameWidth READ frameWidth NOTIFY resultChanged)
    Q_PROPERTY(int frameHeight READ frameHeight NOTIFY resultChanged)
    Q_PROPERTY(bool hasFace READ hasFace NOTIFY resultChanged)
    Q_PROPERTY(bool faceDetected READ faceDetected NOTIFY resultChanged)
    Q_PROPERTY(QRectF faceRect READ faceRect NOTIFY resultChanged)
    Q_PROPERTY(
        bool continuousTracking READ continuousTracking WRITE setContinuousTracking NOTIFY continuousTrackingChanged)
    Q_PROPERTY(GuidanceState guidanceState READ guidanceState NOTIFY resultChanged)
    Q_PROPERTY(Pose detectedPose READ detectedPose NOTIFY resultChanged)
    Q_PROPERTY(bool noFace READ noFace NOTIFY resultChanged)
    Q_PROPERTY(bool multipleFaces READ multipleFaces NOTIFY resultChanged)
    Q_PROPERTY(bool framingSuitable READ framingSuitable NOTIFY resultChanged)
    Q_PROPERTY(FaceFinding faceFinding READ faceFinding NOTIFY resultChanged)
    Q_PROPERTY(int faceCount READ faceCount NOTIFY resultChanged)
    Q_PROPERTY(Position position READ position NOTIFY resultChanged)
    Q_PROPERTY(Distance distance READ distance NOTIFY resultChanged)
    Q_PROPERTY(Quality brightness READ brightness NOTIFY resultChanged)
    Q_PROPERTY(Quality contrast READ contrast NOTIFY resultChanged)
    Q_PROPERTY(Quality sharpness READ sharpness NOTIFY resultChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QString resultSummary READ resultSummary NOTIFY resultChanged)
    Q_PROPERTY(QString guidanceText READ guidanceText NOTIFY resultChanged)
    Q_PROPERTY(QString errorCode READ errorCode NOTIFY stateChanged)
    Q_PROPERTY(quint64 generation READ generation NOTIFY stateChanged)

  public:
    enum class State
    {
        Idle,
        Starting,
        Analyzing,
        Complete,
        Failed,
    };
    Q_ENUM(State)

    enum class FaceFinding
    {
        Unknown,
        NoFace,
        OneFace,
        MultipleFaces,
    };
    Q_ENUM(FaceFinding)

    enum class Position
    {
        Unknown,
        Centered,
        OffCenter,
    };
    Q_ENUM(Position)

    enum class Distance
    {
        Unknown,
        Suitable,
        TooFar,
        TooClose,
    };
    Q_ENUM(Distance)

    enum class Quality
    {
        Unknown,
        Low,
        Suitable,
        High,
    };
    Q_ENUM(Quality)

    enum class GuidanceState
    {
        Unknown,
        NoFace,
        MultipleFaces,
        TooFar,
        TooClose,
        OffCenter,
        PoorLighting,
        Blurred,
        WrongPose,
        Ready,
    };
    Q_ENUM(GuidanceState)

    enum class Pose
    {
        Unknown,
        Frontal,
        Left,
        Right,
        Tilt,
        Natural,
    };
    Q_ENUM(Pose)

    explicit VisionAnalysisSession(CameraPreviewSession *previewSession, QObject *parent = nullptr);
    VisionAnalysisSession(CameraPreviewSession *previewSession, QString workerPath, QObject *parent);
    VisionAnalysisSession(CameraPreviewSession *previewSession, QString workerPath,
                          QProcessEnvironment workerEnvironment, QObject *parent);
    ~VisionAnalysisSession() override;

    [[nodiscard]] State state() const;
    [[nodiscard]] bool busy() const;
    [[nodiscard]] bool canAnalyze() const;
    [[nodiscard]] bool resultAvailable() const;
    [[nodiscard]] int frameWidth() const;
    [[nodiscard]] int frameHeight() const;
    [[nodiscard]] bool hasFace() const;
    [[nodiscard]] bool noFace() const;
    [[nodiscard]] bool multipleFaces() const;
    [[nodiscard]] bool framingSuitable() const;
    [[nodiscard]] bool faceDetected() const;
    [[nodiscard]] QRectF faceRect() const;
    [[nodiscard]] bool continuousTracking() const;
    void setContinuousTracking(bool enabled);
    [[nodiscard]] GuidanceState guidanceState() const;
    [[nodiscard]] Pose detectedPose() const;
    Q_INVOKABLE bool poseMatches(int sampleIndex) const;
    Q_INVOKABLE void startGuidance();
    Q_INVOKABLE void stopGuidance();
    [[nodiscard]] FaceFinding faceFinding() const;
    [[nodiscard]] int faceCount() const;
    [[nodiscard]] Position position() const;
    [[nodiscard]] Distance distance() const;
    [[nodiscard]] Quality brightness() const;
    [[nodiscard]] Quality contrast() const;
    [[nodiscard]] Quality sharpness() const;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QString resultSummary() const;
    [[nodiscard]] QString guidanceText() const;
    [[nodiscard]] QString errorCode() const;
    [[nodiscard]] quint64 generation() const;

    Q_INVOKABLE void analyzeCurrentFrame();
    Q_INVOKABLE void cancelAnalysis();

  Q_SIGNALS:
    void stateChanged();
    void availabilityChanged();
    void resultChanged();
    void continuousTrackingChanged();

  private:
    struct Result
    {
        quint8 faceCount = 0;
        quint8 brightness = 0;
        quint8 contrast = 0;
        quint8 sharpness = 0;
        quint8 flags = 0;
        quint16 x = 0;
        quint16 y = 0;
        quint16 width = 0;
        quint16 height = 0;
        quint16 frameWidth = 0;
        quint16 frameHeight = 0;
        QVector<QPointF> landmarks;
    };

    void startWorker(QByteArray request);
    void writeRequest(QByteArray request);
    void readResponse();
    void processFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void fail(const QString &errorCode);
    void setState(State state, const QString &statusText);
    void clearSensitiveData();
    void clearResult();
    void cancelForLifecycle();
    void stopAnalysis(bool replacementRequested);
    [[nodiscard]] bool parseResponse(QByteArrayView payload, Result *result, QString *errorCode) const;
    void applyResult(const Result &result);
    [[nodiscard]] QString textForError(const QString &errorCode) const;

    CameraPreviewSession *m_previewSession = nullptr;
    QString m_workerPath;
    QProcessEnvironment m_workerEnvironment;
    QProcess *m_process = nullptr;
    QByteArray m_frameBytes;
    QByteArray m_responseBytes;
    State m_state = State::Idle;
    FaceFinding m_faceFinding = FaceFinding::Unknown;
    int m_faceCount = -1;
    Position m_position = Position::Unknown;
    Distance m_distance = Distance::Unknown;
    Quality m_brightness = Quality::Unknown;
    Quality m_contrast = Quality::Unknown;
    Quality m_sharpness = Quality::Unknown;
    QString m_statusText;
    QString m_errorCode;
    quint64 m_generation = 0;
    std::optional<Result> m_pendingResult;
    bool m_ignoringProcessExit = false;
    bool m_responseReceived = false;
    int m_consecutiveVisionErrors = 0;
    bool m_replacementRequested = false;
    quint16 m_requestWidth = 0;
    quint16 m_requestHeight = 0;
    qsizetype m_stderrBytes = 0;
    QTimer m_startupTimer;
    QTimer m_inferenceTimer;
    QTimer m_shutdownTimer;
    QRectF m_faceRect;
    int m_frameWidth = 0;
    int m_frameHeight = 0;
    bool m_continuousTracking = false;
    GuidanceState m_guidanceState = GuidanceState::Unknown;
    Pose m_detectedPose = Pose::Unknown;
    bool m_sessionMode = false;
    bool m_workerStarted = false;
    bool m_requestInFlight = false;
    QTimer m_trackingTimer;
};
