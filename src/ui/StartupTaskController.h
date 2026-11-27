#pragma once

#include <QObject>
#include <QProcess>
#include <QString>

namespace openrgb2::ui
{

/* Windows sign-in task state is read from Task Scheduler, never cached
   as a UI preference. The packaged script owns task registration. */
class StartupTaskController : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool available READ available CONSTANT)
    Q_PROPERTY(bool enabled READ enabled NOTIFY stateChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString error READ error NOTIFY stateChanged)
    /* The user's sign-in task exists but runs another location (a
       moved or deleted install): not "enabled" for this install, and
       not absent either. staleTarget names the path it runs. */
    Q_PROPERTY(bool stale READ stale NOTIFY stateChanged)
    Q_PROPERTY(QString staleTarget READ staleTarget NOTIFY stateChanged)

public:
    explicit StartupTaskController(const QString& installDir,
                                   QObject* parent = nullptr);

    bool available() const;
    bool enabled() const { return enabled_; }
    bool busy() const { return busy_; }
    QString error() const { return error_; }
    bool stale() const { return stale_; }
    QString staleTarget() const { return stale_target_; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void setEnabled(bool on);

signals:
    void stateChanged();

private:
    void Run(const QString& mode);

    QString  install_dir_;
    QString  script_;
    QProcess process_;
    bool     enabled_ = false;
    bool     busy_ = false;
    bool     stale_ = false;
    QString  stale_target_;
    QString  error_;
};

} /* namespace openrgb2::ui */
