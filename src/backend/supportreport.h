// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QObject>
#include <QString>

class CameraPreviewSession;
class SystemState;

class SupportReport final : public QObject
{
    Q_OBJECT

    Q_PROPERTY(QString report READ report NOTIFY reportChanged)
    Q_PROPERTY(QString issueCode READ issueCode NOTIFY reportChanged)
    Q_PROPERTY(QString issueTitle READ issueTitle NOTIFY reportChanged)
    Q_PROPERTY(QString recommendedAction READ recommendedAction NOTIFY reportChanged)
    Q_PROPERTY(QString lastExportPath READ lastExportPath NOTIFY exportChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY exportChanged)
    Q_PROPERTY(bool hasIssue READ hasIssue NOTIFY reportChanged)

  public:
    explicit SupportReport(SystemState *systemState, CameraPreviewSession *cameraPreviewSession = nullptr,
                           QObject *parent = nullptr);

    [[nodiscard]] QString report() const;
    [[nodiscard]] QString issueCode() const;
    [[nodiscard]] QString issueTitle() const;
    [[nodiscard]] QString recommendedAction() const;
    [[nodiscard]] QString lastExportPath() const;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] bool hasIssue() const;

    Q_INVOKABLE void copyReport();
    Q_INVOKABLE bool exportReport();
    void setTransientIssueCode(const QString &code);

    bool exportToDirectory(const QString &directory);
    [[nodiscard]] static QString redactedValue(const QString &value);
    [[nodiscard]] static QString titleForCode(const QString &code);
    [[nodiscard]] static QString actionForCode(const QString &code);

  Q_SIGNALS:
    void reportChanged();
    void exportChanged();

  private:
    void rebuild();
    [[nodiscard]] QString currentIssueCode() const;

    SystemState *m_systemState = nullptr;
    CameraPreviewSession *m_cameraPreviewSession = nullptr;
    QString m_report;
    QString m_issueCode;
    QString m_issueTitle;
    QString m_recommendedAction;
    QString m_lastExportPath;
    QString m_statusText;
    QString m_transientIssueCode;
};
