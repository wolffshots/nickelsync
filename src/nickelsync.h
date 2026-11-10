#ifndef NICKELSYNC_H
#define NICKELSYNC_H

#include <QDateTime>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QWidget>

// NickelSync starts a native Kobo sync when the trigger file changes. A
// NickelMenu item writes the trigger file. Inside a book, it also gets and
// sends the reading position of the open book. The class is a QObject because
// the Nickel signals it listens to are only reachable through string-based
// connect(), which needs real slots.
class NickelSync : public QObject {
    Q_OBJECT

public:
    explicit NickelSync(QObject *parent = nullptr);
    void trigger();

private Q_SLOTS:
    void onConnected();
    void onConnectFailed();
    void onPositionReceived();
    void onSyncStarted();
    void onSyncFinished();
    void onDownloadStarted();
    void onTimeout();
    void onSettled();

private:
    enum State { Idle, Connecting, Pulling, Checking, Requested, Running, Settling };

    bool wire();
    QObject *openBook();
    void startPull();
    void push();
    void startSync();
    void finish(QString const &result, QString const &detail = QString());
    void toast(QString const &main, QString const &sub = QString());
    void log(QString const &line);

    State state = Idle;
    bool wired = false;
    bool pullWired = false;
    bool restoreAirplane = false;
    bool downloadStarted = false;
    QDateTime baseline;
    QPointer<QWidget> view; // the ReadingView of the open book
    QString position;       // the reading position result, for the last toast
    QTimer timeout;
    QTimer settle;
    QElapsedTimer lastTrigger;
};

#endif
