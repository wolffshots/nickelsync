// NickelSync: start a native Kobo sync from a NickelMenu item, also inside a
// book. The sync call is N3SyncManager::silentSync(), the same call that Kobo
// AutoSync uses (github.com/kroshka4aplin/kobo-autosync, MIT). The result
// tracking follows the same model: Nickel sync signals, the SyncManagerBase::fail
// hook, and the LastSyncTime value in Kobo eReader.conf.

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
static void (*SyncManagerBase__fail)(QObject *);

static WirelessManager *(*WirelessManager__sharedInstance)();
static bool (*WirelessManager__connected)(WirelessManager *);

static WirelessWorkflowManager *(*WirelessWorkflowManager__sharedInstance)();
static void (*WirelessWorkflowManager__connectWirelessSilently)(WirelessWorkflowManager *);
static bool (*WirelessWorkflowManager__isAirplaneMode)(WirelessWorkflowManager *);
static void (*WirelessWorkflowManager__setAirplaneMode)(WirelessWorkflowManager *, bool);

static MainWindowController *(*MainWindowController__sharedInstance)();
static void (*MainWindowController__toast)(MainWindowController *, QString const &, QString const &, int);

static NickelSync *controller = nullptr;

static QDateTime lastSyncTime() {
    QSettings conf(QStringLiteral(NICKEL_CONF), QSettings::IniFormat);
    return conf.value(QStringLiteral("ApplicationPreferences/LastSyncTime")).toDateTime();
}

NickelSync::NickelSync(QObject *parent) : QObject(parent) {
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
    if (!sync || !wm) {
        log("ERROR: could not get the sync or wireless manager");
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
    failed = false;
    baseline = lastSyncTime();
    log(QString("CONFIG: ConnectTimeout=%1 RestoreWifi=%2 wifi_off=%3 last_sync=%4")
            .arg(connectTimeout).arg(restoreWifi).arg(airplane).arg(baseline.toString(Qt::ISODate)));

    if (WirelessManager__connected(WirelessManager__sharedInstance())) {
        startSync();
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

void NickelSync::startSync() {
    state = Requested;
    log("REQUESTING: N3SyncManager::silentSync");
    toast("Syncing");
    timeout.start(START_TIMEOUT_MS);
    N3SyncManager__silentSync(N3SyncManager__sharedInstance());
}

void NickelSync::onConnected() {
    if (state != Connecting)
        return;
    log("CONNECTED");
    startSync();
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

void NickelSync::syncFailed(QObject *manager) {
    if (manager != N3SyncManager__sharedInstance() || (state != Requested && state != Running))
        return;
    log("FAIL: SyncManagerBase::fail");
    failed = true;
}

void NickelSync::onSettled() {
    if (state != Settling)
        return;
    QDateTime stamp = lastSyncTime();
    log("LAST_SYNC: " + stamp.toString(Qt::ISODate));
    if (stamp.isValid() && (!baseline.isValid() || stamp > baseline))
        finish("Sync done");
    else if (failed)
        finish("Sync failed");
    else
        finish("Sync finished");
}

void NickelSync::onTimeout() {
    switch (state) {
    case Connecting:
        finish("Wi-Fi connection timed out");
        break;
    case Requested:
        // The syncStarted signal can be missing on some firmware.
        if (SyncManagerBase__isSyncing(N3SyncManager__sharedInstance())) {
            log("STARTED: isSyncing is true, syncStarted not seen");
            state = Running;
            timeout.start(SYNC_TIMEOUT_MS);
        } else {
            finish(failed ? "Sync failed" : "Sync did not start");
        }
        break;
    case Running:
        finish("Sync timed out");
        break;
    default:
        break;
    }
}

void NickelSync::finish(QString const &result) {
    log("RESULT: " + result);
    toast(result);
    timeout.stop();
    settle.stop();
    state = Idle;
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

extern "C" __attribute__((visibility("default"))) void nickelsync_fail(QObject *self) {
    if (controller)
        controller->syncFailed(self);
    SyncManagerBase__fail(self);
}

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
    // Optional: without it, a failed sync shows "Sync finished".
    { .sym = "_ZN15SyncManagerBase4failEv", .sym_new = "nickelsync_fail", .lib = "libnickel.so.1.0.0", .out = nh_symoutptr(SyncManagerBase__fail), .desc = "SyncManagerBase::fail()", .optional = true },
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
    {0},
};
// clang-format on

NickelHook(.init = nickelsync_init, .info = &NickelSyncInfo, .hook = NickelSyncHook, .dlsym = NickelSyncDlsym, .uninstall = nickelsync_uninstall);
