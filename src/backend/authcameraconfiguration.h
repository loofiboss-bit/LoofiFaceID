// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QVariantList>

// No device paths are properties, invokable arguments, diagnostics or QML roles.
class AuthCameraConfiguration final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool busy READ busy NOTIFY changed)
    Q_PROPERTY(QVariantList devices READ devices NOTIFY changed)
    Q_PROPERTY(QString selectedToken READ selectedToken NOTIFY changed)
    Q_PROPERTY(State state READ state NOTIFY changed)
    Q_PROPERTY(QString message READ message NOTIFY changed)
    Q_PROPERTY(QString selectionText READ selectionText NOTIFY changed)
  public:
    enum class State
    {
        Unknown,
        Automatic,
        Selected,
        Failed
    };
    Q_ENUM(State)
    explicit AuthCameraConfiguration(QObject *parent = nullptr);
    // Test seam: the executable paths are C++-only and never environment/QML configured.
    AuthCameraConfiguration(QString helperPath, QString authorizationPath, QObject *parent);
    [[nodiscard]] bool available() const;
    [[nodiscard]] bool busy() const;
    [[nodiscard]] QVariantList devices() const;
    [[nodiscard]] QString selectedToken() const;
    [[nodiscard]] State state() const;
    [[nodiscard]] QString message() const;
    [[nodiscard]] QString selectionText() const;
    Q_INVOKABLE void refresh();
    Q_INVOKABLE void apply(const QString &token);
    Q_INVOKABLE void reset();
  Q_SIGNALS:
    void changed();

  private:
    void start(bool mutation, const QString &operation, const QByteArray &input = {});
    void finished(int exitCode, QProcess::ExitStatus exitStatus);
    bool parseList(const QByteArray &output);
    void fail(const QString &message);
    QString m_helperPath;
    QString m_authorizationPath;
    QProcess m_process;
    QTimer m_timeout;
    QByteArray m_output;
    QByteArray m_errors;
    QByteArray m_input;
    QHash<QString, QByteArray> m_paths;
    QVariantList m_devices;
    QString m_selectedToken;
    QString m_message;
    State m_state = State::Unknown;
    bool m_available = false;
    bool m_mutation = false;
    bool m_timedOut = false;
    QByteArray m_expected;
};
