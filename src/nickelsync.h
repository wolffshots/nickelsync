#ifndef NICKELSYNC_H
#define NICKELSYNC_H

#include <QDateTime>
#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

// NickelSync starts a native Kobo sync when the trigger file changes. A
// NickelMenu item writes the trigger file. The class is a QObject because the
// Nickel signals it listens to are only reachable through string-based
// connect(), which needs real slots.
class NickelSync : public QObject {
    Q_OBJECT

public:
    explicit NickelSync(QObject *parent = nullptr);
    void trigger();

private Q_SLOTS:
    void onConnected();
    void onConnectFailed();
    void onSyncStarted();
    void onSyncFinished();
    void onDownloadStarted();
    void onTimeout();
    void onSettled();

private:
    enum State { Idle, Connecting, Requested, Running, Settling };

    bool wire();
    void startSync();
    void finish(QString const &result, QString const &detail = QString());
    void toast(QString const &main, QString const &sub = QString());
    void log(QString const &line);

    State state = Idle;
    bool wired = false;
    bool restoreAirplane = false;
    bool downloadStarted = false;
    QDateTime baseline;
    QTimer timeout;
    QTimer settle;
    QElapsedTimer lastTrigger;
};

#endif
