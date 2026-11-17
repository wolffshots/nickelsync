// NickelSync: start a native Kobo sync from a page that an icon in the reading
// menu opens, or from a NickelMenu item. The sync call is
// N3SyncManager::silentSync(), the same call that Kobo AutoSync uses (github.com/kroshka4aplin/kobo-autosync, MIT). The result
// tracking uses Nickel sync signals and the LastSyncTime value in
// Kobo eReader.conf.
//
// The full sync does not send or get the position of the open book. Nickel
// sends it only when the user leaves the book. So inside a book, NickelSync
// first gets the position from the server through the reader slot onResume().
// Nickel then shows its "Sync to last page read?" prompt if the server is
// newer. If no prompt shows, NickelSync sends the position with the
// ReadingView signals saveBookmark() and sendBookmark(Volume), which Nickel
// emits when the user leaves a book. With QuickSync=true in the config,
// NickelSync stops after the position sync. If it gets no position from the
// server, or no book is open, it runs the full sync.
//
// autosync.cc can also start a sync when a book loads or the device goes to
// sleep. See the comment at the top of that file.
//
// The icon: an application event filter waits for the show event of the
// ReadingMenuView. Then it puts a TouchLabel into the bottomHorizontalLayout
// bar, before the settingsIcon, as NickelHardcover does. On 4.46 that bar is
// the top bar of the reading menu, and settingsIcon is the overflow icon. A
// tap opens the NickelSync page, a full-screen N3Dialog. The page also shows
// the server stats of the open book (serverstats.cc).
//
// With Hardcover=true in the config, the page also has a Hardcover button. It
// opens a read-only Hardcover page with its own OAuth sign-in. See hardcover.h.
//
// Boot safety: the mod hooks no Nickel function. Until a book or a reading
// menu shows, it only watches the trigger file and checks the type of each Qt
// event in the event filters. So a fault in this mod cannot run at boot after
// the NickelHook failsafe window, and cannot cause a boot loop.

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileSystemWatcher>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QPixmap>
#include <QScreen>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <QVBoxLayout>

#include <NickelHook.h>

#include "hardcover.h"
#include "nickel.h"
#include "nickelsync.h"
#include "settings.h"
#include "autosync.h"
#include "serverstats.h"

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
// QuickSync skips the full sync, so the run can end while Nickel still uploads
// the position. This is the wait before Wi-Fi goes off again.
// ponytail: a fixed wait. Nickel uploads in a worker thread that a mod cannot
// watch without a hook. An upload that takes longer stops when Wi-Fi goes off.
static const int UPLOAD_WAIT_MS = 15 * 1000;
static const int TOAST_MS = 3000;

// The icon uses the Nickel status bar sync image.
#define NS_ICON      "nickelsyncIcon"
#define NS_PIX       ":/images/statusbar/sync.png"

typedef QObject N3SyncManager;
typedef QObject WirelessManager;
typedef QObject WirelessWorkflowManager;
typedef QObject MainWindowController;

static N3SyncManager *(*N3SyncManager__sharedInstance)();
static void (*N3SyncManager__silentSync)(N3SyncManager *);
static bool (*SyncManagerBase__isSyncing)(N3SyncManager const *);

WirelessManager *(*WirelessManager__sharedInstance)();
bool (*WirelessManager__connected)(WirelessManager *);

static WirelessWorkflowManager *(*WirelessWorkflowManager__sharedInstance)();
static void (*WirelessWorkflowManager__connectWirelessSilently)(WirelessWorkflowManager *);
static bool (*WirelessWorkflowManager__isAirplaneMode)(WirelessWorkflowManager *);
static void (*WirelessWorkflowManager__setAirplaneMode)(WirelessWorkflowManager *, bool);

// Other source files also use this function and the page functions below
// (nickel.h), so they are not static.
MainWindowController *(*MainWindowController__sharedInstance)();
static void (*MainWindowController__toast)(MainWindowController *, QString const &, QString const &, int);

// Optional. If a firmware lacks them, NickelSync only runs the full sync.
QWidget *(*MainWindowController__viewWithObjectName)(MainWindowController const *, QString const &);
void const *(*ReadingView__getVolume)(void const *); // returns Volume const&

// Optional. If a firmware lacks them, the reading menu has no NickelSync icon.
// The object sizes are the sizes that NickelHardcover allocates.
void (*TouchLabel__constructor)(QLabel *, QWidget *, QFlags<Qt::WindowType>);
static void (*TouchLabel__setHitStateEnabled)(QLabel *, bool);
void (*N3ButtonLabel__constructor)(QLabel *, QWidget *);
QWidget *(*N3DialogFactory__getDialog)(QWidget *, bool);
void (*N3Dialog__setTitle)(QWidget *, QString const &);
QWidget *(*MainWindowController__pushView)(MainWindowController *, QWidget *);

// Content__getId (server stats and Hardcover), Content__getTitle and
// Content__getAttribution are optional. hardcover.cc defines them (nickel.h).

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

void NickelSync::trigger(Mode mode, Origin from) {
    // inotify can deliver one write as several events.
    if (lastTrigger.isValid() && lastTrigger.elapsed() < 1000)
        return;
    lastTrigger.start();

    if (state != Idle) {
        if (from == Manual)
            toast("Sync already running");
        return;
    }
    origin = from;

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

    int connectTimeout = settingInt("ConnectTimeout");
    bool restoreWifi = settingBool("RestoreWifi");
    quickSync = mode == FromConfig ? settingBool("QuickSync") : mode == Quick;

    WirelessWorkflowManager *wfm = WirelessWorkflowManager__sharedInstance();
    bool airplane = WirelessWorkflowManager__isAirplaneMode(wfm);
    // The server stats of the page can have turned Wi-Fi on.
    restoreAirplane = (restoreWifi && airplane) || statsAirplane;
    statsAirplane = false;
    downloadStarted = false;
    position.clear();
    baseline = lastSyncTime();
    log(QString("CONFIG: ConnectTimeout=%1 RestoreWifi=%2 QuickSync=%3 wifi_off=%4 last_sync=%5 origin=%6")
            .arg(connectTimeout).arg(restoreWifi).arg(quickSync).arg(airplane).arg(baseline.toString(Qt::ISODate)).arg(int(origin)));

    bool connected = WirelessManager__connected(WirelessManager__sharedInstance());
    // With Wi-Fi on, Nickel itself gets the position when a book loads.
    if (origin == AutoOpen && connected) {
        log("SKIP: Wi-Fi is on, so Nickel gets the reading position");
        return;
    }
    // The sleep screen hides the book soon, so find the book now.
    if (origin == AutoSleep)
        openBook();
    if (connected) {
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
    // On sleep the user leaves the book, so send the position as Nickel does
    // when the user closes a book. No one can answer a prompt, so do not pull.
    if (origin == AutoSleep) {
        if (view)
            push();
        if (view && quickSync) {
            state = Checking; // so that finish() waits for the upload
            finish(position);
        } else {
            startSync();
        }
        return;
    }
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
        if (!quickSync) {
            startSync();
        } else {
            log("QUICK: no full sync");
            // push() always sets position, so it is empty only if the prompt shows.
            finish(position.isEmpty() ? QStringLiteral("Reading position received") : position);
        }
        break;
    case Uploading:
        state = Idle;
        restoreAirplaneMode();
        Q_EMIT ended();
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
    lastResult = detail.isEmpty() ? result : result + ", " + detail;
    if (origin != Manual)
        lastResult = (origin == AutoOpen ? "Auto-sync on open: " : "Auto-sync on sleep: ") + lastResult;
    toast(result, detail);
    timeout.stop();
    settle.stop();
    view.clear();
    // Only a quick sync finishes in the Checking state. Nickel can still be
    // uploading the position, so wait before Wi-Fi goes off, or before the
    // device can sleep.
    if (state == Checking && (restoreAirplane || origin == AutoSleep)) {
        log("UPLOADING: waiting for the upload");
        state = Uploading;
        timeout.start(UPLOAD_WAIT_MS);
        return;
    }
    state = Idle;
    restoreAirplaneMode();
    Q_EMIT ended();
}

// restoreAirplaneMode turns Wi-Fi off again if it was off before the sync.
void NickelSync::restoreAirplaneMode() {
    // Do not turn Wi-Fi off while Nickel syncs or downloads books.
    if (restoreAirplane && !downloadStarted && !SyncManagerBase__isSyncing(N3SyncManager__sharedInstance())) {
        log("WIFI: turning Wi-Fi off again");
        WirelessWorkflowManager__setAirplaneMode(WirelessWorkflowManager__sharedInstance(), true);
    }
    restoreAirplane = false;
}

void NickelSync::toast(QString const &main, QString const &sub) {
    MainWindowController *mwc = MainWindowController__sharedInstance();
    // A toast blocks taps on the book, so an automatic sync shows none.
    if (!mwc || origin != Manual)
        return;
    // Nickel shows one toast at a time. If a toast is on the screen, Nickel
    // shows this one later. eventFilter moves it when it shows.
    toasts << main;
    MainWindowController__toast(mwc, main, sub, TOAST_MS);
}

// eventFilter is on the application from init. It acts only on show events:
// it adds the icon to a reading menu, and it moves NickelSync toasts.
bool NickelSync::eventFilter(QObject *obj, QEvent *event) {
    if (event->type() != QEvent::Show)
        return false;
    if (obj->inherits("ReadingMenuView"))
        addIcon(qobject_cast<QWidget *>(obj));
    else if (!toasts.isEmpty() && obj->inherits("N3ToastView"))
        moveToast(obj);
    return false;
}

// addIcon puts the NickelSync icon into the bottomHorizontalLayout bar of the
// reading menu, before the last icon. NickelHardcover finds the bar and
// inserts its icon in the same way. On 4.46 this bar is the lower row of the
// header, at the top of the screen. settingsIcon is the gear, and the last
// icon is the overflow icon. So the mod icons stay together, between the gear
// and the overflow icon. The menu shows many times, so addIcon adds the icon
// only once.
void NickelSync::addIcon(QWidget *menuView) {
    if (!menuView || !TouchLabel__constructor || !TouchLabel__setHitStateEnabled || !N3ButtonLabel__constructor ||
        !N3DialogFactory__getDialog || !N3Dialog__setTitle || !MainWindowController__pushView)
        return;
    QWidget *root = menuView->parentWidget() ? menuView->parentWidget() : menuView;
    if (root->findChild<QLabel *>(QStringLiteral(NS_ICON)))
        return;
    QHBoxLayout *bar = root->findChild<QHBoxLayout *>(QStringLiteral("bottomHorizontalLayout"));
    QLabel *settings = root->findChild<QLabel *>(QStringLiteral("settingsIcon"));
    // The settingsIcon check makes sure that this is the bar of the icons.
    // NickelSync uses settingsIcon only for the icon height.
    if (!bar || !settings || bar->indexOf(settings) < 0) {
        nh_log("NickelSync: ICON: bottomHorizontalLayout or settingsIcon not found");
        return;
    }
    // A widget in a layout has no final size before the first show, so use
    // the size of the settings image.
    int height = settings->pixmap() ? settings->pixmap()->height() : settings->height();

    QLabel *icon = reinterpret_cast<QLabel *>(calloc(1, 128));
    TouchLabel__constructor(icon, settings->parentWidget(), 0);
    TouchLabel__setHitStateEnabled(icon, false);
    icon->setObjectName(QStringLiteral(NS_ICON));
    icon->setPixmap(QPixmap(QStringLiteral(NS_PIX)).scaledToHeight(height, Qt::SmoothTransformation));
    connect(icon, SIGNAL(tapped(bool)), this, SLOT(showPage()));
    int index = bar->count() - 1;
    bar->insertWidget(index, icon);
    nh_log("NickelSync: ICON: added at index %d, height %d", index, height);
}

// pageLabel adds a label with an object name for the page style sheet.
static QLabel *pageLabel(QVBoxLayout *layout, char const *name, QString const &text) {
    QLabel *l = new QLabel(text);
    l->setObjectName(QString::fromLatin1(name));
    l->setWordWrap(true);
    layout->addWidget(l);
    return l;
}

// openBookId returns the ContentID of the open book, or an empty string. Call
// it before the page hides the book.
static QString openBookId() {
    if (!Content__getId || !MainWindowController__viewWithObjectName || !ReadingView__getVolume)
        return QString();
    MainWindowController *mwc = MainWindowController__sharedInstance();
    QWidget *rv = mwc ? MainWindowController__viewWithObjectName(mwc, QStringLiteral("ReadingView")) : nullptr;
    void *reading = rv && rv->isVisible() ? rv->qt_metacast("ReadingView") : nullptr;
    return reading ? Content__getId(ReadingView__getVolume(reading)) : QString();
}

// showPage opens the NickelSync page over the book. It follows the Dialog
// class of NickelHardcover: Nickel draws the title and the close button.
void NickelSync::showPage() {
    if (page)
        return;
    QWidget *icon = qobject_cast<QWidget *>(sender());
    // On the device the window of the icon is the main window, which fills the
    // screen. The sim window is smaller than its desktop screen.
    QSize screen = icon ? icon->window()->size() : QApplication::primaryScreen()->geometry().size();
    // Sizes in pixels for a 1264 pixel wide screen (Libra 2, Libra Colour),
    // as in the Label sizes of kpm.
    auto px = [screen](int at1264) { return at1264 * screen.width() / 1264; };

    QWidget *content = new QWidget();
    content->setStyleSheet(QStringLiteral("QLabel#nsKey { font-size: %1px; color: #666666; }"
                                          "QLabel#nsValue { font-size: %2px; }"
                                          "QLabel#nsDesc { font-size: %1px; }"
                                          "QLabel#nsHeading { font-size: %2px; font-weight: bold; }")
                               .arg(px(30))
                               .arg(px(38)));
    QVBoxLayout *l = new QVBoxLayout(content);
    l->setContentsMargins(px(75), px(40), px(75), px(40));
    l->setSpacing(px(12));

    QDateTime last = lastSyncTime();
    pageLabel(l, "nsKey", QStringLiteral("Last sync"));
    pageLabel(l, "nsValue", last.isValid() ? last.toString(QStringLiteral("d MMMM yyyy, HH:mm")) : QStringLiteral("Never"));
    l->addSpacing(px(24));
    pageLabel(l, "nsKey", QStringLiteral("Last result"));
    pageLabel(l, "nsValue", state != Idle             ? QStringLiteral("A sync is running")
                            : lastResult.isEmpty() ? QStringLiteral("No sync since the last restart")
                                                   : lastResult);

    struct { char const *text; char const *desc; Mode mode; } buttons[] = {
        {"Sync now", "Syncs the reading position of this book, then your library.", Full},
        {"Sync reading position", "Syncs only the reading position of this book. Faster.", Quick},
    };
    for (auto const &b : buttons) {
        l->addSpacing(px(56));
        QLabel *button = reinterpret_cast<QLabel *>(calloc(1, 512));
        N3ButtonLabel__constructor(button, content);
        button->setProperty("primaryButton", true);
        button->setProperty("nsMode", int(b.mode));
        button->setText(QString::fromUtf8(b.text));
        connect(button, SIGNAL(tapped(bool)), this, SLOT(onPageButton()));
        l->addWidget(button, 0, Qt::AlignLeft);
        pageLabel(l, "nsDesc", QString::fromUtf8(b.desc));
    }
    l->addSpacing(px(56));
    // Settings, and the button of the Hardcover page, in one row.
    QHBoxLayout *more = new QHBoxLayout();
    more->setSpacing(px(24));
    l->addLayout(more);
    QLabel *settingsButton = SettingsPage::addButton(more, content);
    // The settings page can turn Hardcover on or off, so the button is always
    // there. onSubPageClosed shows or hides it.
    QLabel *hardcoverButton = Hardcover::instance()->addButton(more, screen);
    hcButton = hardcoverButton;
    hcButton->setVisible(Hardcover::enabled());
    // These buttons open a page over this one. With the hit state, Nickel
    // draws a tapped button as pressed, and the button stays pressed below the
    // new page. Back then shows this page and redraws the button: a second
    // e-ink update. The reading menu icon has no hit state for the same reason.
    TouchLabel__setHitStateEnabled(settingsButton, false);
    TouchLabel__setHitStateEnabled(hardcoverButton, false);
    more->addStretch(1);
    // Below the buttons, so that the buttons do not move when the stats load.
    l->addSpacing(px(56));
    ServerStats *stats = new ServerStats(openBookId(), px(12));
    l->addWidget(stats);
    l->addStretch(1);

    page = N3DialogFactory__getDialog(content, true);
    N3Dialog__setTitle(page, QStringLiteral("NickelSync"));
    page->setFixedSize(screen);
    followDarkMode(page, content);
    MainWindowController__pushView(MainWindowController__sharedInstance(), page);
    connect(page, SIGNAL(closeTapped()), page, SLOT(deleteLater()));
    connect(page, SIGNAL(backTapped()), page, SLOT(deleteLater()));
    // Queued, so that Nickel shows the book again before a sync starts.
    connect(page, SIGNAL(destroyed()), this, SLOT(onPageClosed()), Qt::QueuedConnection);
    page->show();
    nh_log("NickelSync: PAGE: opened, %dx%d", screen.width(), screen.height());
    startStats(stats);
}

// startStats loads the server stats. If Wi-Fi is off, it connects Wi-Fi first,
// and onPageClosed turns it off again.
void NickelSync::startStats(ServerStats *stats) {
    if (!stats->needsNetwork())
        return;
    // Without the managers, try the request. It fails with an error if Wi-Fi is off.
    WirelessManager *wm = WirelessManager__sharedInstance();
    WirelessWorkflowManager *wfm = WirelessWorkflowManager__sharedInstance();
    if (!wm || !wfm || WirelessManager__connected(wm)) {
        stats->load();
        return;
    }
    stats->waitForWifi(wm, settingInt("ConnectTimeout") * 1000);
    // A running sync connects Wi-Fi itself, and restores it.
    if (state != Idle)
        return;
    if (WirelessWorkflowManager__isAirplaneMode(wfm)) {
        statsAirplane = settingBool("RestoreWifi");
        WirelessWorkflowManager__setAirplaneMode(wfm, false);
    }
    nh_log("NickelSync: STATS: connecting Wi-Fi");
    WirelessWorkflowManager__connectWirelessSilently(wfm);
}

// onPageButton closes the page and starts the sync when the page is gone. The
// book is hidden below the page, and the reading position sync needs it
// visible: openBook() checks it, and Nickel shows its prompt over it.
void NickelSync::onPageButton() {
    QObject *button = sender();
    if (!button || !page)
        return;
    pending = Mode(button->property("nsMode").toInt());
    syncOnClose = true;
    page->deleteLater();
}

void openSubPage(QWidget *sub) {
    MainWindowController__pushView(MainWindowController__sharedInstance(), sub);
    QObject::connect(sub, SIGNAL(backTapped()), sub, SLOT(deleteLater()));
    if (controller)
        controller->addSubPage(sub);
    else
        QObject::connect(sub, SIGNAL(closeTapped()), sub, SLOT(deleteLater()));
    sub->show();
}

// followDarkMode does for a NickelSync page what Nickel does for the reading
// view. ReadingView::showEvent sets the property darkMode on a child widget of
// the reading view. A rule of the Nickel style sheet then paints that widget
// and its children white on black:
//   *[darkMode=true], *[darkMode=true] * { color: white; background-color: black; }
// Nickel has no such rule for its own N3Dialog pages, so the page needs more
// changes. Nickel fills a primary button in code, not in the style sheet, and
// its text is hard to see on black. So primary buttons become bordered
// buttons, and bordered buttons get a white border. The icons of the title
// bar (back and close) get inverted pixmaps.
void followDarkMode(QWidget *page, QWidget *content) {
    if (!MainWindowController__viewWithObjectName)
        return;
    QWidget *rv = MainWindowController__viewWithObjectName(MainWindowController__sharedInstance(),
                                                           QStringLiteral("ReadingView"));
    if (!rv)
        return;
    bool dark = false;
    for (QWidget *w : rv->findChildren<QWidget *>())
        if (w->property("darkMode").toBool()) {
            dark = true;
            break;
        }
    if (!dark)
        return;
    page->setProperty("darkMode", true);
    // Hardcover::button() does the same for the buttons that it adds later.
    for (QLabel *b : content->findChildren<QLabel *>())
        if (b->property("primaryButton").toBool()) {
            b->setProperty("primaryButton", false);
            b->setProperty("borderedButton", true);
        }
    // Ui_N3Dialog gives the frame mainFrame its own style sheet with a white
    // background. That sheet wins over the page sheet, and the frame shows in
    // the gaps of the title row, for example below the back and close icons.
    if (QWidget *frame = page->findChild<QWidget *>(QStringLiteral("mainFrame")))
        frame->setStyleSheet(frame->styleSheet() + QStringLiteral("\n#mainFrame { background-color: black; }"));
    // setStyleSheet also polishes the page and its children again, so that
    // the new property applies to the widgets that the style has polished.
    page->setStyleSheet(QStringLiteral("N3ButtonLabel[borderedButton=true] { border-color: white; }"));
    for (QLabel *l : page->findChildren<QLabel *>()) {
        if (content->isAncestorOf(l) || !l->pixmap() || l->pixmap()->isNull())
            continue;
        // Qt 5.2 inverts a premultiplied image without a conversion, which
        // makes the transparent pixels white. So convert it first.
        QImage icon = l->pixmap()->toImage().convertToFormat(QImage::Format_ARGB32);
        icon.invertPixels();
        l->setPixmap(QPixmap::fromImage(icon));
    }
    nh_log("NickelSync: PAGE: dark mode");
}

// addSubPage makes the close button of sub close the whole NickelSync stack.
void NickelSync::addSubPage(QWidget *sub) {
    subPages.append(sub);
    connect(sub, SIGNAL(closeTapped()), this, SLOT(closeAll()));
    connect(sub, SIGNAL(destroyed()), this, SLOT(onSubPageClosed()), Qt::QueuedConnection);
}

// closeAll closes the sub-pages and the NickelSync page, back to the book.
void NickelSync::closeAll() {
    nh_log("NickelSync: PAGE: close all");
    for (QPointer<QWidget> const &sub : subPages)
        if (sub)
            sub->deleteLater();
    subPages.clear();
    if (page)
        page->deleteLater();
}

// onSubPageClosed shows or hides the Hardcover button, because the settings
// page can change Hardcover.
void NickelSync::onSubPageClosed() {
    subPages.removeAll(QPointer<QWidget>());
    if (hcButton)
        hcButton->setVisible(Hardcover::enabled());
}

// onPageClosed starts the sync of a page button. Then, if no sync runs, it
// turns Wi-Fi off again if the server stats turned it on.
void NickelSync::onPageClosed() {
    if (syncOnClose) {
        syncOnClose = false;
        trigger(pending);
    }
    if (statsAirplane && state == Idle) {
        statsAirplane = false;
        restoreAirplane = true;
        restoreAirplaneMode();
    }
}

// moveToast moves each NickelSync toast to the top of the screen before
// Nickel draws it. Nickel centers a toast, so it covers the centered "Sync to
// last page read?" prompt.
void NickelSync::moveToast(QObject *obj) {
    // N3ToasterController::displayToast() makes the toast a top-level window,
    // centers it with move() and then calls show(). Qt sends the show event
    // before the window shows, so this move comes before the first draw.
    QWidget *toastView = qobject_cast<QWidget *>(obj);
    QLabel *text = toastView ? toastView->findChild<QLabel *>(QStringLiteral("primaryText")) : nullptr;
    // A toast of Nickel itself stays in the center.
    if (text && toasts.removeOne(text->text())) {
        toastView->move(toastView->x(), toastView->x() / 2); // the top gap is half the side gap
        log("TOAST_MOVED: " + text->text());
    }
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

    // Without the filter there is no icon, and the toasts stay in the center.
    if (QCoreApplication *app = QCoreApplication::instance())
        app->installEventFilter(controller);
    else
        nh_log("NickelSync: ERROR: no application, no icon");
    new AutoSync(controller);
    return 0;
}

static bool nickelsync_uninstall() {
    nh_delete_file(NS_NM);
    nh_delete_file(NS_CONFIG);
    nh_delete_file(NS_CONFIG ".corrupt");
    nh_delete_file(NS_LOG);
    nh_delete_file(NS_DIR "/hardcover.ini");
    nh_delete_dir(NS_DIR);
    return true;
}

static struct nh_info NickelSyncInfo = (struct nh_info){
    .name = "NickelSync",
    .desc = "Start a native Kobo sync from the reading menu",
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
    { .name = "_ZN10TouchLabelC1EP7QWidget6QFlagsIN2Qt10WindowTypeEE",       .out = nh_symoutptr(TouchLabel__constructor),                  .desc = "reading menu icon", .optional = true },
    { .name = "_ZN10TouchLabel18setHitStateEnabledEb",                       .out = nh_symoutptr(TouchLabel__setHitStateEnabled),           .desc = "reading menu icon", .optional = true },
    { .name = "_ZN13N3ButtonLabelC1EP7QWidget",                              .out = nh_symoutptr(N3ButtonLabel__constructor),               .desc = "reading menu icon", .optional = true },
    { .name = "_ZN15N3DialogFactory9getDialogEP7QWidgetb",                   .out = nh_symoutptr(N3DialogFactory__getDialog),               .desc = "reading menu icon", .optional = true },
    { .name = "_ZN8N3Dialog8setTitleERK7QString",                            .out = nh_symoutptr(N3Dialog__setTitle),                       .desc = "reading menu icon", .optional = true },
    { .name = "_ZN20MainWindowController8pushViewEP7QWidget",                .out = nh_symoutptr(MainWindowController__pushView),           .desc = "reading menu icon", .optional = true },
    { .name = "_ZN22N3PowerWorkflowManager14sharedInstanceEv",              .out = nh_symoutptr(N3PowerWorkflowManager__sharedInstance),   .desc = "auto-sync on sleep", .optional = true },
    { .name = "_ZNK22N3PowerWorkflowManager8sleepingEv",                     .out = nh_symoutptr(N3PowerWorkflowManager__sleeping),         .desc = "auto-sync on sleep", .optional = true },
    { .name = "_ZN20ScopedSuspendDisableC1Ev",                               .out = nh_symoutptr(ScopedSuspendDisable__constructor),        .desc = "auto-sync on sleep", .optional = true },
    { .name = "_ZN12PowerManager14sharedInstanceEv",                         .out = nh_symoutptr(PowerManager__sharedInstance),             .desc = "auto-sync on sleep", .optional = true },
    { .name = "_ZNK7Content5getIdEv",                                        .out = nh_symoutptr(Content__getId),                           .desc = "server stats, Hardcover page", .optional = true },
    { .name = "_ZNK7Content8getTitleEv",                                     .out = nh_symoutptr(Content__getTitle),                        .desc = "Hardcover page", .optional = true },
    { .name = "_ZNK7Content14getAttributionEv",                              .out = nh_symoutptr(Content__getAttribution),                  .desc = "Hardcover page", .optional = true },
    {0},
};
// clang-format on

NickelHook(.init = nickelsync_init, .info = &NickelSyncInfo, .hook = NickelSyncHook, .dlsym = NickelSyncDlsym, .uninstall = nickelsync_uninstall);
