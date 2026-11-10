// NickelSync: start a native Kobo sync from a NickelMenu item, also inside a
// book. The sync call is N3SyncManager::silentSync(), the same call that Kobo
// AutoSync uses (github.com/kroshka4aplin/kobo-autosync, MIT). The result
// tracking uses Nickel sync signals and the LastSyncTime value in
// Kobo eReader.conf.
//
// The full sync does not send or get the position of the open book. Nickel
// sends it only when the user leaves the book. So inside a book, NickelSync
// first gets the position from the server through the reader slot onResume().
// Nickel then shows its "Sync to last page read?" prompt if the server is
// newer. If no prompt shows, NickelSync sends the position with the
// ReadingView signals saveBookmark() and sendBookmark(Volume), which Nickel
// emits when the user leaves a book.
//
// Boot safety: the mod hooks no Nickel function. Until the first tap, it only
// watches the trigger file. So a fault in this mod cannot run at boot after the
// NickelHook failsafe window, and cannot cause a boot loop.

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileSystemWatcher>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTextStream>

#include <NickelHook.h>

#include "nickelsync.h"

#define NS_DIR       "/mnt/onboard/.adds/nickelsync"
#define NS_CONFIG    NS_DIR "/config.ini"
#define NS_LOG       NS_DIR "/log.txt"
#define NS_XFLAG     NS_DIR "/uninstall"
#define NS_NM        "/mnt/onboard/.adds/nm/nickelsync"
#define NS_TRIGGER   "/tmp/nickelsync"
#define NICKEL_CONF  "/mnt/onboard/.kobo/Kobo/Kobo eReader.conf"

// Time for the server to answer the reading state request. Nickel sends no
// request for a book that it does not sync, for example a sideloaded book.
static const int PULL_TIMEOUT_MS = 20 * 1000;
// Delay after the answer, before the check for the prompt. Nickel opens the
// prompt in the same call chain as the answer, so a short delay is enough.
static const int PROMPT_WAIT_MS = 1500;
// Time for Nickel to emit syncStarted after the silentSync call.
static const int START_TIMEOUT_MS = 20 * 1000;
// Time for a started sync to emit finished.
static const int SYNC_TIMEOUT_MS = 5 * 60 * 1000;
// Delay after finished, so that Nickel can write LastSyncTime first.
static const int SETTLE_MS = 3 * 1000;
static const int TOAST_MS = 3000;

typedef QObject N3SyncManager;
typedef QObject WirelessManager;
typedef QObject WirelessWorkflowManager;
typedef QObject MainWindowController;

static N3SyncManager *(*N3SyncManager__sharedInstance)();
static void (*N3SyncManager__silentSync)(N3SyncManager *);
static bool (*SyncManagerBase__isSyncing)(N3SyncManager const *);

static WirelessManager *(*WirelessManager__sharedInstance)();
static bool (*WirelessManager__connected)(WirelessManager *);

static WirelessWorkflowManager *(*WirelessWorkflowManager__sharedInstance)();
static void (*WirelessWorkflowManager__connectWirelessSilently)(WirelessWorkflowManager *);
static bool (*WirelessWorkflowManager__isAirplaneMode)(WirelessWorkflowManager *);
static void (*WirelessWorkflowManager__setAirplaneMode)(WirelessWorkflowManager *, bool);

static MainWindowController *(*MainWindowController__sharedInstance)();
static void (*MainWindowController__toast)(MainWindowController *, QString const &, QString const &, int);

// Optional. If a firmware lacks them, NickelSync only runs the full sync.
static QWidget *(*MainWindowController__viewWithObjectName)(MainWindowController const *, QString const &);
static void const *(*ReadingView__getVolume)(void const *); // returns Volume const&

static NickelSync *controller = nullptr;

static QDateTime lastSyncTime() {
    QSettings conf(QStringLiteral(NICKEL_CONF), QSettings::IniFormat);
    return conf.value(QStringLiteral("ApplicationPreferences/LastSyncTime")).toDateTime();
}

NickelSync::NickelSync(QObject *parent) : QObject(parent) {
    // QElapsedTimer has no constructor in Qt 5.2. Without this call it holds
    // heap garbage, and trigger() can ignore every tap as a repeat.
    lastTrigger.invalidate();
    timeout.setSingleShot(true);
    settle.setSingleShot(true);
    settle.setInterval(SETTLE_MS);
    connect(&timeout, SIGNAL(timeout()), this, SLOT(onTimeout()));
    connect(&settle, SIGNAL(timeout()), this, SLOT(onSettled()));
}

// wire connects the Nickel signals. It runs on the first trigger, not at init,
// because the Nickel singletons can be missing while Nickel starts.
bool NickelSync::wire() {
    if (wired)
        return true;
    QObject *sync = N3SyncManager__sharedInstance();
    QObject *wm = WirelessManager__sharedInstance();
    if (!sync || !wm || !WirelessWorkflowManager__sharedInstance()) {
        log("ERROR: could not get the sync or wireless managers");
        return false;
    }
    // A failed connect is not fatal. The timeouts still end each attempt.
    QStringList missing;
    if (!connect(wm, SIGNAL(networkConnected()), this, SLOT(onConnected())))
        missing << "networkConnected";
    if (!connect(wm, SIGNAL(networkFailedToConnect()), this, SLOT(onConnectFailed())))
        missing << "networkFailedToConnect";
    if (!connect(sync, SIGNAL(syncStarted(SyncState::SyncType)), this, SLOT(onSyncStarted())))
        missing << "syncStarted";
    if (!connect(sync, SIGNAL(finished()), this, SLOT(onSyncFinished())))
        missing << "finished";
    if (!connect(sync, SIGNAL(downloadStarted(Volume)), this, SLOT(onDownloadStarted())))
        missing << "downloadStarted";
    // The reader listens to the same signal. This connection is older, so
    // onPositionReceived runs before Nickel decides about the prompt.
    pullWired = connect(sync, SIGNAL(bookmarkRequestSuccess(Volume)), this, SLOT(onPositionReceived()));
    if (!pullWired)
        missing << "bookmarkRequestSuccess";
    if (!missing.isEmpty())
        log("WARN: signals not connected: " + missing.join(", "));
    wired = true;
    return true;
}

void NickelSync::trigger() {
    // inotify can deliver one write as several events.
    if (lastTrigger.isValid() && lastTrigger.elapsed() < 1000)
        return;
    lastTrigger.start();

    if (state != Idle) {
        toast("Sync already running");
        return;
    }

    QFile::remove(NS_LOG);
    log("TRIGGER");
    if (!wire()) {
        toast("Sync failed", "Nickel is not ready");
        return;
    }
    if (SyncManagerBase__isSyncing(N3SyncManager__sharedInstance())) {
        log("SKIP: Nickel is already syncing");
        toast("Sync already running");
        return;
    }

    QSettings cfg(QStringLiteral(NS_CONFIG), QSettings::IniFormat);
    int connectTimeout = qBound(10, cfg.value("NickelSync/ConnectTimeout", 60).toInt(), 180);
    bool restoreWifi = cfg.value("NickelSync/RestoreWifi", true).toBool();

    WirelessWorkflowManager *wfm = WirelessWorkflowManager__sharedInstance();
    bool airplane = WirelessWorkflowManager__isAirplaneMode(wfm);
    restoreAirplane = restoreWifi && airplane;
    downloadStarted = false;
    position.clear();
    baseline = lastSyncTime();
    log(QString("CONFIG: ConnectTimeout=%1 RestoreWifi=%2 wifi_off=%3 last_sync=%4")
            .arg(connectTimeout).arg(restoreWifi).arg(airplane).arg(baseline.toString(Qt::ISODate)));

    if (WirelessManager__connected(WirelessManager__sharedInstance())) {
        startPull();
        return;
    }
    state = Connecting;
    log("CONNECTING");
    toast("Connecting to Wi-Fi");
    if (airplane)
        WirelessWorkflowManager__setAirplaneMode(wfm, false);
    WirelessWorkflowManager__connectWirelessSilently(wfm);
    timeout.start(connectTimeout * 1000);
}

// openBook returns the reader of the open book and sets view to its
// ReadingView. It returns null if no kepub book is open.
QObject *NickelSync::openBook() {
    view.clear();
    if (!pullWired || !MainWindowController__viewWithObjectName || !ReadingView__getVolume) {
        log("BOOK: this firmware lacks the functions for the reading position");
        return nullptr;
    }
    MainWindowController *mwc = MainWindowController__sharedInstance();
    QWidget *rv = mwc ? MainWindowController__viewWithObjectName(mwc, QStringLiteral("ReadingView")) : nullptr;
    // viewWithObjectName also finds a ReadingView below the current view.
    if (!rv || !rv->isVisible() || !rv->inherits("ReadingView")) {
        log("BOOK: no open book");
        return nullptr;
    }
    for (QObject *child : rv->findChildren<QObject *>()) {
        if (child->inherits("KepubBookReaderBase")) {
            view = rv;
            return child;
        }
    }
    log("BOOK: the open book is not a kepub");
    return nullptr;
}

// startPull gets the reading position before the push. A push first would
// give the server the Kobo position with a new time, so the server would never
// be newer and Nickel would never show the prompt.
void NickelSync::startPull() {
    QObject *reader = openBook();
    if (!reader) {
        startSync();
        return;
    }
    state = Pulling;
    log("PULLING: KepubBookReaderBase::onResume");
    toast("Syncing");
    timeout.start(PULL_TIMEOUT_MS);
    // Nickel calls the same slot when the device wakes. It sends GET .../state.
    if (!QMetaObject::invokeMethod(reader, "onResume", Qt::DirectConnection)) {
        log("PULL_FAILED: onResume not found");
        position = "Reading position not synced";
        startSync();
    }
}

void NickelSync::onPositionReceived() {
    if (state != Pulling)
        return;
    log("PULLED: waiting for the prompt");
    state = Checking;
    timeout.start(PROMPT_WAIT_MS);
}

// promptVisible reports a visible Nickel confirmation dialog, such as "Sync to
// last page read?" or "Finished this book?".
static bool promptVisible() {
    for (QWidget *w : QApplication::allWidgets())
        if (w->isVisible() && w->inherits("ConfirmationDialog"))
            return true;
    return false;
}

// push emits the two ReadingView signals that Nickel emits when the user leaves
// a book. saveBookmark() writes the current page to the database.
// sendBookmark(Volume) sends it to the server with PUT .../state.
void NickelSync::push() {
    QWidget *rv = view.data();
    // moc generates qt_metacast, so the ReadingView pointer is correct for any
    // base class layout. On 4.46.23852 the QWidget base is at offset 0.
    void *reading = rv ? rv->qt_metacast("ReadingView") : nullptr;
    if (!reading) {
        log("PUSH_SKIPPED: the book is closed");
        position = "Reading position not synced";
        return;
    }
    if (!QMetaObject::invokeMethod(rv, "saveBookmark", Qt::DirectConnection) ||
        !QMetaObject::invokeMethod(rv, "sendBookmark", Qt::DirectConnection,
                                   QGenericArgument("Volume", ReadingView__getVolume(reading)))) {
        log("PUSH_FAILED: ReadingView signals not found");
        position = "Reading position not synced";
        return;
    }
    log("PUSHED: saveBookmark, sendBookmark");
    position = "Reading position sent";
}

void NickelSync::startSync() {
    // startPull already showed the toast.
    bool pulled = state == Pulling || state == Checking;
    state = Requested;
    log("REQUESTING: N3SyncManager::silentSync");
    if (!pulled)
        toast("Syncing");
    timeout.start(START_TIMEOUT_MS);
    N3SyncManager__silentSync(N3SyncManager__sharedInstance());
}

void NickelSync::onConnected() {
    if (state != Connecting)
        return;
    log("CONNECTED");
    startPull();
}

void NickelSync::onConnectFailed() {
    if (state != Connecting)
        return;
    log("CONNECT_FAILED");
    finish("Could not connect to Wi-Fi");
}

void NickelSync::onSyncStarted() {
    if (state != Requested)
        return;
    log("STARTED");
    state = Running;
    timeout.start(SYNC_TIMEOUT_MS);
}

void NickelSync::onSyncFinished() {
    if (state != Requested && state != Running)
        return;
    log("FINISHED: waiting for LastSyncTime");
    state = Settling;
    timeout.stop();
    settle.start();
}

void NickelSync::onDownloadStarted() {
    if (state == Idle)
        return;
    log("DOWNLOAD_STARTED: Wi-Fi stays on");
    downloadStarted = true;
}

void NickelSync::onSettled() {
    if (state != Settling)
        return;
    QDateTime stamp = lastSyncTime();
    log("LAST_SYNC: " + stamp.toString(Qt::ISODate));
    if (stamp.isValid() && (!baseline.isValid() || stamp > baseline))
        finish("Sync done", position);
    else
        finish("Sync not confirmed", "Nickel did not update its last sync time");
}

void NickelSync::onTimeout() {
    switch (state) {
    case Connecting:
        finish("Wi-Fi connection timed out");
        break;
    case Pulling:
        // Without the server position, a push could replace a newer position
        // from another device. So do not push.
        log("PULL_TIMEOUT: no reading state from the server, no push");
        position = "Reading position not synced";
        startSync();
        break;
    case Checking:
        if (promptVisible()) {
            log("PROMPT: the server has a newer position, no push");
        } else {
            push();
        }
        startSync();
        break;
    case Requested:
        // The syncStarted signal can be missing on some firmware.
        if (SyncManagerBase__isSyncing(N3SyncManager__sharedInstance())) {
            log("STARTED: isSyncing is true, syncStarted not seen");
            state = Running;
            timeout.start(SYNC_TIMEOUT_MS);
        } else {
            finish("Sync did not start");
        }
        break;
    case Running:
        finish("Sync timed out");
        break;
    default:
        break;
    }
}

void NickelSync::finish(QString const &result, QString const &detail) {
    log("RESULT: " + result);
    toast(result, detail);
    timeout.stop();
    settle.stop();
    state = Idle;
    view.clear();
    // Do not turn Wi-Fi off while Nickel syncs or downloads books.
    if (restoreAirplane && !downloadStarted && !SyncManagerBase__isSyncing(N3SyncManager__sharedInstance())) {
        log("WIFI: turning Wi-Fi off again");
        WirelessWorkflowManager__setAirplaneMode(WirelessWorkflowManager__sharedInstance(), true);
    }
    restoreAirplane = false;
}

void NickelSync::toast(QString const &main, QString const &sub) {
    MainWindowController *mwc = MainWindowController__sharedInstance();
    if (mwc)
        MainWindowController__toast(mwc, main, sub, TOAST_MS);
}

void NickelSync::log(QString const &line) {
    nh_log("NickelSync: %s", qPrintable(line));
    QDir().mkpath(NS_DIR);
    QFile f(NS_LOG);
    if (f.open(QIODevice::Append | QIODevice::Text))
        QTextStream(&f) << QDateTime::currentDateTime().toString(Qt::ISODate) << " " << line << "\n";
}

// ---- NickelHook --------------------------------------------------------

static int nickelsync_init() {
    controller = new NickelSync();

    // The trigger file is on tmpfs, so it is missing after each boot.
    { QFile f(NS_TRIGGER); f.open(QIODevice::WriteOnly); }
    static QFileSystemWatcher *watcher = new QFileSystemWatcher(QStringList() << NS_TRIGGER);
    QObject::connect(watcher, &QFileSystemWatcher::fileChanged, [](QString const &path) {
        // The watch stops if a tool replaces the file. Add it again.
        if (!watcher->files().contains(path)) {
            QFile f(path);
            f.open(QIODevice::WriteOnly);
            f.close();
            watcher->addPath(path);
        }
        controller->trigger();
    });
    nh_log("NickelSync: watching %s", NS_TRIGGER);
    return 0;
}

static bool nickelsync_uninstall() {
    nh_delete_file(NS_NM);
    nh_delete_file(NS_CONFIG);
    nh_delete_file(NS_LOG);
    nh_delete_dir(NS_DIR);
    return true;
}

static struct nh_info NickelSyncInfo = (struct nh_info){
    .name = "NickelSync",
    .desc = "Start a native Kobo sync from a NickelMenu item",
    .uninstall_flag = nullptr,
    // Delete this shipped file and reboot to uninstall.
    .uninstall_xflag = NS_XFLAG,
    .failsafe_delay = 15,
};

// clang-format off
static struct nh_hook NickelSyncHook[] = {
    // No hooks on purpose. See the boot safety note at the top of this file.
    {0},
};

static struct nh_dlsym NickelSyncDlsym[] = {
    { .name = "_ZN13N3SyncManager14sharedInstanceEv",                        .out = nh_symoutptr(N3SyncManager__sharedInstance) },
    { .name = "_ZN13N3SyncManager10silentSyncEv",                            .out = nh_symoutptr(N3SyncManager__silentSync) },
    { .name = "_ZNK15SyncManagerBase9isSyncingEv",                           .out = nh_symoutptr(SyncManagerBase__isSyncing) },
    { .name = "_ZN15WirelessManager14sharedInstanceEv",                      .out = nh_symoutptr(WirelessManager__sharedInstance) },
    { .name = "_ZN15WirelessManager9connectedEv",                            .out = nh_symoutptr(WirelessManager__connected) },
    { .name = "_ZN23WirelessWorkflowManager14sharedInstanceEv",              .out = nh_symoutptr(WirelessWorkflowManager__sharedInstance) },
    { .name = "_ZN23WirelessWorkflowManager23connectWirelessSilentlyEv",     .out = nh_symoutptr(WirelessWorkflowManager__connectWirelessSilently) },
    { .name = "_ZN23WirelessWorkflowManager14isAirplaneModeEv",              .out = nh_symoutptr(WirelessWorkflowManager__isAirplaneMode) },
    { .name = "_ZN23WirelessWorkflowManager15setAirplaneModeEb",             .out = nh_symoutptr(WirelessWorkflowManager__setAirplaneMode) },
    { .name = "_ZN20MainWindowController14sharedInstanceEv",                 .out = nh_symoutptr(MainWindowController__sharedInstance) },
    { .name = "_ZN20MainWindowController5toastERK7QStringS2_i",              .out = nh_symoutptr(MainWindowController__toast) },
    { .name = "_ZNK20MainWindowController18viewWithObjectNameERK7QString",   .out = nh_symoutptr(MainWindowController__viewWithObjectName), .desc = "reading position sync", .optional = true },
    { .name = "_ZNK11ReadingView9getVolumeEv",                               .out = nh_symoutptr(ReadingView__getVolume),                   .desc = "reading position sync", .optional = true },
    {0},
};
// clang-format on

NickelHook(.init = nickelsync_init, .info = &NickelSyncInfo, .hook = NickelSyncHook, .dlsym = NickelSyncDlsym, .uninstall = nickelsync_uninstall);
