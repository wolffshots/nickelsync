#ifndef AUTOSYNC_H
#define AUTOSYNC_H

#include <QObject>
#include <QPointer>
#include <QTimer>

class NickelSync;

// Optional Nickel functions for the sleep trigger. The dlsym table in
// nickelsync.cc fills them.
extern QObject *(*N3PowerWorkflowManager__sharedInstance)();
extern bool (*N3PowerWorkflowManager__sleeping)(QObject const *);
extern void (*ScopedSuspendDisable__constructor)(QObject *);
extern QObject *(*PowerManager__sharedInstance)();

// AutoSync starts a sync when a book loads (AutoSyncOnOpen) or when the device
// goes to sleep (AutoSyncOnSleep).
class AutoSync : public QObject {
    Q_OBJECT

public:
    explicit AutoSync(NickelSync *sync);
    bool eventFilter(QObject *obj, QEvent *event) override;

private Q_SLOTS:
    void onBookLoaded();
    void onBookClosing();
    void onAboutToSleep();
    void onHoldLimit();
    void release();
    void checkSuspended();

private:
    void wireSleep();

    NickelSync *sync;
    QPointer<QObject> wired;  // the last ReadingView with connected signals
    QPointer<QObject> synced; // the ReadingView that had its auto-sync for the open book
    bool sleepWired = false;
    QObject *guard = nullptr; // a Nickel ScopedSuspendDisable while a sleep sync runs
    QTimer holdLimit;
};

#endif
