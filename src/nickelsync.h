#ifndef NICKELSYNC_H
#define NICKELSYNC_H

#include <QDateTime>
#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <QWidget>

class ServerStats;

// NickelSync starts a native Kobo sync from its icon in the reading menu, or
// when the trigger file changes. A NickelMenu item can write the trigger file.
// Inside a book, it also gets and sends the reading position of the open
// book. The class is a QObject because
// the Nickel signals it listens to are only reachable through string-based
// connect(), which needs real slots.
class NickelSync : public QObject {
    Q_OBJECT

public:
    // FromConfig uses QuickSync from the config. Full and Quick override it.
    enum Mode { FromConfig, Full, Quick };
    // Origin is what started a sync. An automatic sync shows no toasts.
    enum Origin { Manual, AutoOpen, AutoSleep };

    explicit NickelSync(QObject *parent = nullptr);
    void trigger(Mode mode = FromConfig, Origin from = Manual);
    bool busy() const { return state != Idle; }
    bool eventFilter(QObject *obj, QEvent *event) override;
    void log(QString const &line);

Q_SIGNALS:
    // ended comes when a sync that trigger() started is done.
    void ended();

private Q_SLOTS:
    void showPage();
    void onPageButton();
    void onPageClosed();
    void onConnected();
    void onConnectFailed();
    void onPositionReceived();
    void onSyncStarted();
    void onSyncFinished();
    void onDownloadStarted();
    void onTimeout();
    void onSettled();

private:
    enum State { Idle, Connecting, Pulling, Checking, Requested, Running, Settling, Uploading };

    bool wire();
    void addIcon(QWidget *menuView);
    void startStats(ServerStats *stats);
    void moveToast(QObject *obj);
    QObject *openBook();
    void startPull();
    void push();
    void startSync();
    void finish(QString const &result, QString const &detail = QString());
    void restoreAirplaneMode();
    void toast(QString const &main, QString const &sub = QString());

    State state = Idle;
    Origin origin = Manual;
    bool wired = false;
    bool pullWired = false;
    bool restoreAirplane = false;
    bool downloadStarted = false;
    bool quickSync = false; // inside a book, sync only the reading position
    QDateTime baseline;
    QPointer<QWidget> view; // the ReadingView of the open book
    QString position;       // the reading position result, for the last toast
    QStringList toasts;     // the main texts of toasts that Nickel did not show yet
    QPointer<QWidget> page; // the open NickelSync page
    Mode pending = FromConfig; // the sync to start when the page closes
    bool syncOnClose = false;  // a page button was tapped
    bool statsAirplane = false; // the server stats turned airplane mode off
    QString lastResult;     // the result of the last sync since Nickel started
    QTimer timeout;
    QTimer settle;
    QElapsedTimer lastTrigger;
};

#endif
