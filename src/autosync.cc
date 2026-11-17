// AutoSync: start a sync without a tap. It hooks no Nickel function.
//
// Book open (AutoSyncOnOpen=true): the application event filter catches the
// show event of each ReadingView and connects its readerDoneLoading() signal.
// When a book has loaded, AutoSync starts a quick sync, which gets the reading
// position from the server and can show the "Sync to last page read?" prompt.
// It runs once for each book open. If Wi-Fi is on, Nickel gets the position
// itself when the book loads, so NickelSync does nothing.
//
// Sleep (AutoSyncOnSleep=true): N3PowerWorkflowManager emits aboutToSleep()
// before it shows the sleep screen. AutoSync then builds a Nickel
// ScopedSuspendDisable object, which vetoes each suspend while it exists.
// Nickel itself uses one during power off. AutoSync sends the position of the
// open book and runs the full sync, or only sends the position with
// QuickSync=true. When the sync ends, or after HOLD_MS, AutoSync deletes the
// object, so the device can suspend again. If the device is still awake on
// the sleep screen SUSPEND_CHECK_MS later, AutoSync asks PowerManager to
// suspend.
//
// No toasts show for an automatic sync. The NickelSync page shows the result.

#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFile>

#include <NickelHook.h>

#include <cstdlib>

#include "autosync.h"
#include "nickelsync.h"
#include "settings.h"

// The longest time that AutoSync keeps the device awake on the sleep screen.
static const int HOLD_MS = 60 * 1000;
// The wait after the release, before the check that the device sleeps.
static const int SUSPEND_CHECK_MS = 10 * 1000;
// Below this battery charge in percent, there is no sync on sleep.
static const int MIN_BATTERY = 10;
// ponytail: Nickel puts ScopedSuspendDisable on the stack, so it is small (a
// QObject is 8 bytes). The size is not in the symbols. 64 bytes is a margin.
static const size_t GUARD_SIZE = 64;

QObject *(*N3PowerWorkflowManager__sharedInstance)();
bool (*N3PowerWorkflowManager__sleeping)(QObject const *);
void (*ScopedSuspendDisable__constructor)(QObject *);
QObject *(*PowerManager__sharedInstance)();

// batteryLevel returns the battery charge in percent, or -1 if it is unknown.
static int batteryLevel() {
    QDir dir(QStringLiteral("/sys/class/power_supply"));
    for (QString const &name : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QFile type(dir.filePath(name + "/type"));
        if (!type.open(QIODevice::ReadOnly) || type.readAll().trimmed() != "Battery")
            continue;
        QFile capacity(dir.filePath(name + "/capacity"));
        if (capacity.open(QIODevice::ReadOnly))
            return capacity.readAll().trimmed().toInt();
    }
    return -1;
}

AutoSync::AutoSync(NickelSync *sync) : QObject(sync), sync(sync) {
    holdLimit.setSingleShot(true);
    connect(&holdLimit, SIGNAL(timeout()), this, SLOT(onHoldLimit()));
    connect(sync, SIGNAL(ended()), this, SLOT(release()));
    if (QCoreApplication *app = QCoreApplication::instance())
        app->installEventFilter(this);
}

// eventFilter acts only on the show event of a ReadingView. The view shows
// again after each view over it, so it connects each view once.
bool AutoSync::eventFilter(QObject *obj, QEvent *event) {
    if (event->type() != QEvent::Show || obj == wired.data() || !obj->inherits("ReadingView"))
        return false;
    wired = obj;
    if (!connect(obj, SIGNAL(readerDoneLoading()), this, SLOT(onBookLoaded())) ||
        !connect(obj, SIGNAL(readerWillClose()), this, SLOT(onBookClosing())))
        nh_log("NickelSync: AUTO: ReadingView signals not connected");
    // A book is open, so Nickel has started. N3PowerWorkflowManager exists.
    wireSleep();
    return false;
}

void AutoSync::onBookLoaded() {
    QObject *view = sender();
    if (!view || view == synced || !settingBool("AutoSyncOnOpen"))
        return;
    synced = view;
    nh_log("NickelSync: AUTO: book loaded");
    sync->trigger(NickelSync::Quick, NickelSync::AutoOpen);
}

void AutoSync::onBookClosing() { synced.clear(); }

// wireSleep connects aboutToSleep() once. It waits for the first book, so it
// calls no Nickel function while Nickel starts.
// ponytail: so sleep before the first book after a boot has no auto-sync.
void AutoSync::wireSleep() {
    if (sleepWired)
        return;
    sleepWired = true;
    if (!N3PowerWorkflowManager__sharedInstance || !ScopedSuspendDisable__constructor) {
        nh_log("NickelSync: AUTO: this firmware lacks the functions for the sleep trigger");
        return;
    }
    QObject *power = N3PowerWorkflowManager__sharedInstance();
    // Direct, so that the sync starts before Nickel shows the sleep screen
    // over the book.
    if (!power || !connect(power, SIGNAL(aboutToSleep()), this, SLOT(onAboutToSleep()), Qt::DirectConnection))
        nh_log("NickelSync: AUTO: aboutToSleep not connected");
}

void AutoSync::onAboutToSleep() {
    if (guard || !settingBool("AutoSyncOnSleep"))
        return;
    // Nickel also sends the device to sleep when the battery is low.
    int battery = batteryLevel();
    if (battery >= 0 && battery < MIN_BATTERY) {
        nh_log("NickelSync: AUTO: battery at %d%%, no sync on sleep", battery);
        return;
    }
    guard = reinterpret_cast<QObject *>(calloc(1, GUARD_SIZE));
    ScopedSuspendDisable__constructor(guard);
    holdLimit.start(HOLD_MS);
    nh_log("NickelSync: AUTO: sleep, suspend held");
    sync->trigger(NickelSync::FromConfig, NickelSync::AutoSleep);
    // trigger() can return at once, for example if Nickel already syncs.
    if (!sync->busy())
        release();
}

void AutoSync::onHoldLimit() {
    sync->log("AUTO: hold limit reached");
    release();
}

// release deletes the guard, so that the device can suspend again.
void AutoSync::release() {
    if (!guard)
        return;
    holdLimit.stop();
    delete guard; // the virtual destructor of ScopedSuspendDisable, then free()
    guard = nullptr;
    sync->log("AUTO: suspend allowed");
    QTimer::singleShot(SUSPEND_CHECK_MS, this, SLOT(checkSuspended()));
}

// checkSuspended runs SUSPEND_CHECK_MS after the release. Timers do not run
// while the device is suspended, so if the device suspended, this runs after
// the wake and sleeping() is false.
void AutoSync::checkSuspended() {
    QObject *power = N3PowerWorkflowManager__sharedInstance();
    if (!power || !N3PowerWorkflowManager__sleeping || !PowerManager__sharedInstance ||
        !N3PowerWorkflowManager__sleeping(power))
        return;
    sync->log("AUTO: still awake on the sleep screen, calling PowerManager::suspend");
    QMetaObject::invokeMethod(PowerManager__sharedInstance(), "suspend", Qt::QueuedConnection);
}
