// Desktop stand-ins for Nickel. See nickelstub.h.
#include "nickelstub.h"

#include <QApplication>
#include <QDateTime>
#include <QDebug>
#include <QFont>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>

#include <cstring>
#include <new>

SimOptions simOptions;
SimStats simStats;
int ScopedSuspendDisable::count = 0;

#define NICKEL_CONF "/mnt/onboard/.kobo/Kobo/Kobo eReader.conf"

static N3SyncManager *syncManager;
static WirelessManager *wirelessManager;
static WirelessWorkflowManager *workflowManager;
static MainWindowController *mainWindowController;
static N3PowerWorkflowManager *powerWorkflowManager;
static PowerManager *powerManager;
static QWidget *mainWindow;
static ReadingView *readingView;
static Volume volume;

static void simLog(QString const &line) { qInfo().noquote() << "nickel(sim):" << line; }

// ---- sync and Wi-Fi ----------------------------------------------------

void N3SyncManager::silentSync() {
    simLog("N3SyncManager::silentSync");
    simStats.syncs++;
    syncing = true;
    QTimer::singleShot(500, this, [this] { Q_EMIT syncStarted(SyncState::Full); });
    QTimer::singleShot(simOptions.syncMs, this, [this] {
        QSettings conf(QStringLiteral(NICKEL_CONF), QSettings::IniFormat);
        conf.setValue(QStringLiteral("ApplicationPreferences/LastSyncTime"), QDateTime::currentDateTime());
        conf.sync();
        syncing = false;
        simLog("sync finished");
        Q_EMIT finished();
    });
}

void WirelessWorkflowManager::connectWirelessSilently() {
    simLog("WirelessWorkflowManager::connectWirelessSilently");
    QTimer::singleShot(simOptions.connectMs, this, [] {
        if (simOptions.wifi == SimOptions::WifiFail) {
            Q_EMIT wirelessManager->networkFailedToConnect();
            return;
        }
        wirelessManager->connected = true;
        Q_EMIT wirelessManager->networkConnected();
    });
}

// ---- sleep ---------------------------------------------------------------

void N3PowerWorkflowManager::sleep() {
    simLog("N3PowerWorkflowManager: aboutToSleep");
    sleeping = true;
    Q_EMIT aboutToSleep();
    QLabel *screen = new QLabel(QStringLiteral("Sleeping"), mainWindow);
    screen->setObjectName(QStringLiteral("sleepScreen"));
    screen->setAlignment(Qt::AlignCenter);
    screen->setStyleSheet(QStringLiteral("background: white; font-size: 40px;"));
    screen->setGeometry(mainWindow->rect());
    readingView->hide();
    screen->show();
    QTimer::singleShot(1000, powerManager, SLOT(suspend()));
}

void PowerManager::suspend() {
    if (simStats.suspended)
        return;
    if (ScopedSuspendDisable::count > 0) {
        simStats.vetoes++;
        simLog("PowerManager::suspend: vetoed");
        if (simOptions.suspendRetry)
            QTimer::singleShot(1000, this, SLOT(suspend()));
        return;
    }
    simLog("PowerManager::suspend: suspended");
    simStats.suspended = true;
}

ScopedSuspendDisable::ScopedSuspendDisable() {
    count++;
    simLog("ScopedSuspendDisable: created");
}

ScopedSuspendDisable::~ScopedSuspendDisable() {
    count--;
    simLog("ScopedSuspendDisable: deleted");
}

// ---- main window and toasts --------------------------------------------

static QFont uiFont(int px, bool bold = false) {
    QFont f(QStringLiteral("DejaVu Sans"));
    f.setPixelSize(px);
    f.setBold(bold);
    return f;
}

N3ToastView::N3ToastView(QWidget *parent, QString const &main, QString const &sub) : QFrame(parent) {
    setStyleSheet(QStringLiteral("N3ToastView { background: black; border-radius: 8px; } QLabel { color: white; }"));
    QVBoxLayout *l = new QVBoxLayout(this);
    l->setContentsMargins(28, 18, 28, 18);
    QLabel *primary = new QLabel(main, this);
    primary->setObjectName(QStringLiteral("primaryText"));
    primary->setFont(uiFont(22, true));
    primary->setAlignment(Qt::AlignCenter);
    l->addWidget(primary);
    if (!sub.isEmpty()) {
        QLabel *secondary = new QLabel(sub, this);
        secondary->setFont(uiFont(17));
        secondary->setAlignment(Qt::AlignCenter);
        l->addWidget(secondary);
    }
}

// toast shows one toast at a time, as Nickel does. Nickel makes the toast a
// top-level window, centers it and shows it. The sim makes it a child of the
// screen window, so that move() coordinates are screen coordinates, as on the
// device.
void MainWindowController::toast(QString const &main, QString const &sub, int ms) {
    simStats.toasts++;
    simLog("toast: " + main + (sub.isEmpty() ? QString() : " / " + sub));
    queue.append(qMakePair(qMakePair(main, sub), ms));
    if (!showing)
        showNext();
}

void MainWindowController::showNext() {
    if (queue.isEmpty()) {
        showing = false;
        return;
    }
    showing = true;
    auto next = queue.takeFirst();
    N3ToastView *t = new N3ToastView(mainWindow, next.first.first, next.first.second);
    t->adjustSize();
    t->move((mainWindow->width() - t->width()) / 2, (mainWindow->height() - t->height()) / 2);
    t->show();
    t->raise();
    QTimer::singleShot(next.second, t, [this, t] {
        t->deleteLater();
        showNext();
    });
}

ConfirmationDialog::ConfirmationDialog(QWidget *parent, QString const &title, QString const &text) : QFrame(parent) {
    setStyleSheet(QStringLiteral("ConfirmationDialog { background: white; border: 2px solid black; }"
                                 "QPushButton { background: white; border: 2px solid black; padding: 10px 24px; }"));
    QVBoxLayout *l = new QVBoxLayout(this);
    l->setContentsMargins(32, 28, 32, 28);
    l->setSpacing(18);
    QLabel *t = new QLabel(title, this);
    t->setFont(uiFont(24, true));
    QLabel *x = new QLabel(text, this);
    x->setFont(uiFont(18));
    x->setWordWrap(true);
    QHBoxLayout *buttons = new QHBoxLayout();
    QPushButton *cancel = new QPushButton(QStringLiteral("Cancel"), this);
    QPushButton *accept = new QPushButton(QStringLiteral("Sync"), this);
    cancel->setFont(uiFont(18));
    accept->setFont(uiFont(18, true));
    buttons->addStretch();
    buttons->addWidget(cancel);
    buttons->addWidget(accept);
    l->addWidget(t);
    l->addWidget(x);
    l->addLayout(buttons);
    connect(cancel, &QPushButton::clicked, this, [this] { simLog("prompt: cancel"); deleteLater(); });
    connect(accept, &QPushButton::clicked, this, [this] { simLog("prompt: go to the server position"); deleteLater(); });
    setFixedWidth(parent->width() * 4 / 5);
    adjustSize();
    move((parent->width() - width()) / 2, (parent->height() - height()) / 2);
}

// ---- reader --------------------------------------------------------------

// onResume is the reader slot that Nickel calls when the device wakes. It
// sends GET .../state. The sim answers after pullMs.
void KepubBookReaderBase::onResume() {
    simLog("KepubBookReaderBase::onResume: GET .../state");
    simStats.pulls++;
    if (simOptions.server == SimOptions::ServerNone)
        return;
    QTimer::singleShot(simOptions.pullMs, this, [] {
        Q_EMIT syncManager->bookmarkRequestSuccess(volume);
        if (simOptions.server == SimOptions::ServerNewer) {
            ConfirmationDialog *d = new ConfirmationDialog(
                mainWindow, QStringLiteral("Sync to last page read?"),
                QStringLiteral("You last read this book on another device, at page 212. Go to that page?"));
            d->show();
            d->raise();
        }
    });
}

// icon draws a placeholder icon for a bar of the reading menu.
static QPixmap icon(QString const &glyph, int size) {
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::TextAntialiasing);
    p.setFont(uiFont(size * 2 / 3));
    p.drawText(pm.rect(), Qt::AlignCenter, glyph);
    return pm;
}

// drawn draws the placeholder icons that have no font glyph.
static QPixmap drawn(QString const &kind, int size) {
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(Qt::black, qMax(2, size / 16)));
    int u = size / 8;
    if (kind == "lock") {
        p.drawArc(QRect(3 * u, u, 2 * u, 4 * u), 0, 180 * 16);
        p.drawRoundedRect(QRect(2 * u, 3 * u, 4 * u, 4 * u), u / 2, u / 2);
    } else if (kind == "stats") {
        int heights[] = {3, 5, 2, 4};
        for (int i = 0; i < 4; i++)
            p.fillRect(QRect(2 * u + i * u, 7 * u - heights[i] * u, u / 2 + 1, heights[i] * u), Qt::black);
        p.drawLine(u + u / 2, 7 * u, 7 * u, 7 * u);
    } else if (kind == "battery") {
        p.drawRect(QRect(u / 2, 3 * u, 6 * u, 2 * u));
        p.fillRect(QRect(u, 3 * u + u / 2, 5 * u, u + 1), Qt::black);
        p.fillRect(QRect(6 * u + u / 2, 3 * u + u / 2, u / 2, u), Qt::black);
    } else if (kind == "mod") {
        // A generic mod icon: a rounded square. modIcon() puts a letter on it.
        p.drawRoundedRect(QRect(u + u / 2, u + u / 2, 5 * u, 5 * u), u, u);
    }
    return pm;
}

static const int BAR_ICON = 34;

static QLabel *barIcon(QWidget *parent, char const *name, QPixmap const &pm) {
    QLabel *l = new QLabel(parent);
    l->setObjectName(QString::fromLatin1(name));
    l->setPixmap(pm);
    return l;
}

// modIcon is the icon of another mod for --bar-icons. "hardcover" is the
// NickelHardcover icon. Any other name gets a rounded square with its first
// letter.
static QPixmap modIcon(QString const &name, int size) {
    if (name == QLatin1String("hardcover"))
        return QPixmap(QStringLiteral(":/sim/hardcover.svg")).scaledToHeight(size, Qt::SmoothTransformation);
    QPixmap pm = drawn(QStringLiteral("mod"), size);
    QPainter p(&pm);
    p.setFont(uiFont(size / 3, true));
    p.drawText(pm.rect(), Qt::AlignCenter, name.left(1).toUpper());
    return pm;
}

// ReadingMenuView is the overlay that shows when you tap the center of a page.
// The layout copies the NickelHardcover screenshot of the reading menu
// (projects/NickelHardcover-src/screenshots/menu.png): a header with a status
// row and an icon row, and a footer with the page, a slider and the title.
// The icon row is bottomHorizontalLayout: the lower row of the header. In it,
// settingsIcon is the gear, and the overflow icon is last. The names
// bottomHorizontalLayout and settingsIcon come from NickelHardcover. The other
// names, the icons and the sizes are guesses.
ReadingMenuView::ReadingMenuView(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("ReadingMenuView"));
    QVBoxLayout *l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(0);

    QFrame *header = new QFrame(this);
    header->setObjectName(QStringLiteral("header"));
    header->setStyleSheet(QStringLiteral("#header { background: white; border-bottom: 2px solid black; }"));
    QVBoxLayout *hv = new QVBoxLayout(header);
    hv->setContentsMargins(20, 6, 14, 8);
    hv->setSpacing(4);

    QHBoxLayout *status = new QHBoxLayout();
    QLabel *time = new QLabel(QStringLiteral("12:47 PM"), header);
    time->setFont(uiFont(15));
    status->addWidget(time);
    status->addStretch(1);
    status->addWidget(barIcon(header, "batteryIcon", drawn(QStringLiteral("battery"), 28)));
    hv->addLayout(status);

    QHBoxLayout *bar = new QHBoxLayout();
    bar->setObjectName(QStringLiteral("bottomHorizontalLayout"));
    bar->setSpacing(14);
    QLabel *back = new QLabel(QStringLiteral("Back to Home"), header);
    back->setFont(uiFont(18, true));
    back->setMinimumWidth(1); // a full bar cuts this text, it does not push icons off the screen
    bar->addWidget(barIcon(header, "backIcon", icon(QStringLiteral("\u2190"), BAR_ICON)));
    bar->addWidget(back, 1);
    bar->addWidget(barIcon(header, "lockIcon", drawn(QStringLiteral("lock"), BAR_ICON)));
    bar->addWidget(barIcon(header, "frontLightIcon", icon(QStringLiteral("\u263c"), BAR_ICON)));
    bar->addWidget(barIcon(header, "fontIcon", icon(QStringLiteral("Aa"), BAR_ICON)));
    bar->addWidget(barIcon(header, "statsIcon", drawn(QStringLiteral("stats"), BAR_ICON)));
    bar->addWidget(barIcon(header, "settingsIcon", icon(QStringLiteral("\u2699"), BAR_ICON)));
    bar->addWidget(barIcon(header, "overflowIcon", icon(QStringLiteral("\u22ef"), BAR_ICON)));
    hv->addLayout(bar);

    // Other mods that hook the ReadingMenuView constructor, as NickelHardcover
    // does. NickelHardcover inserts at count() - 1, before the last widget,
    // when Nickel builds the menu. That is before the first show event, where
    // NickelSync adds its icon.
    for (QString const &name : simOptions.barIcons) {
        QLabel *other = new QLabel(header);
        other->setObjectName(QStringLiteral("simIcon_") + name);
        other->setPixmap(modIcon(name, BAR_ICON));
        bar->insertWidget(bar->count() - 1, other);
    }

    QFrame *footer = new QFrame(this);
    footer->setObjectName(QStringLiteral("footer"));
    footer->setStyleSheet(QStringLiteral("#footer { background: white; border-top: 2px solid black; }"
                                         "#slider { background: black; }"));
    QVBoxLayout *fl = new QVBoxLayout(footer);
    fl->setContentsMargins(24, 14, 24, 16);
    fl->setSpacing(14);
    QLabel *progress = new QLabel(QStringLiteral("PAGE 212 OF 618"), footer);
    progress->setFont(uiFont(16));
    progress->setAlignment(Qt::AlignCenter);
    fl->addWidget(progress);
    QFrame *slider = new QFrame(footer);
    slider->setObjectName(QStringLiteral("slider"));
    slider->setFixedHeight(2);
    fl->addWidget(slider);
    QHBoxLayout *fb = new QHBoxLayout();
    fb->setSpacing(14);
    QLabel *book = new QLabel(QStringLiteral("The Lighthouse Keeper"), footer);
    book->setFont(uiFont(18, true));
    fb->addWidget(barIcon(footer, "tocIcon", icon(QStringLiteral("\u2630"), BAR_ICON)));
    fb->addWidget(book, 1);
    fb->addWidget(barIcon(footer, "annotationsIcon", icon(QStringLiteral("\u25a4"), BAR_ICON)));
    fb->addWidget(barIcon(footer, "searchIcon", icon(QStringLiteral("\u2315"), BAR_ICON)));
    fl->addLayout(fb);

    l->addWidget(header);
    l->addStretch();
    l->addWidget(footer);
}

// A tap between the header and the footer closes the menu, as in Nickel.
void ReadingMenuView::mouseReleaseEvent(QMouseEvent *) { hide(); }

ReadingView::ReadingView(QWidget *parent) : QWidget(parent) {
    setObjectName(QStringLiteral("ReadingView"));
    new KepubBookReaderBase(this);
    connect(this, &ReadingView::saveBookmark, [] { simLog("ReadingView::saveBookmark"); });
    connect(this, &ReadingView::sendBookmark, [] {
        simLog("ReadingView::sendBookmark: PUT .../state");
        simStats.pushes++;
    });
}

void ReadingView::showMenu() {
    if (!menu)
        menu = new ReadingMenuView(this);
    menu->setGeometry(rect());
    menu->show();
    menu->raise();
}

void ReadingView::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.fillRect(rect(), Qt::white);
    p.setFont(uiFont(21));
    QString text = QStringLiteral(
        "The lamp turned through the night as it always had, and Ada counted the "
        "beats between each sweep of light across the water. Seven, and then the "
        "dark. Seven, and then the dark again.\n\n"
        "In the morning the supply boat did not come. She wrote it in the log in "
        "her small square hand, the way her father had taught her, and then she "
        "climbed the stairs to clean the glass.\n\n"
        "From the gallery she could see the whole of the bay: the grey roofs of the "
        "village, the long arm of the harbour wall, and far out, where the sea "
        "turned from green to slate, a single sail that did not move.\n\n"
        "She watched it for an hour. It did not come closer and it did not go "
        "away, and when the fog came in at noon it was still there, a pale "
        "triangle in the white.");
    p.drawText(rect().adjusted(48, 56, -48, -80), Qt::TextWordWrap, text);
    p.setFont(uiFont(15));
    p.drawText(rect().adjusted(0, 0, 0, -24), Qt::AlignHCenter | Qt::AlignBottom, QStringLiteral("212 of 618"));
}

// A tap in the center third opens the reading menu, as in Nickel.
void ReadingView::mouseReleaseEvent(QMouseEvent *event) {
    QRect center(width() / 3, height() / 3, width() / 3, height() / 3);
    if (center.contains(event->pos()))
        showMenu();
}

// ---- widgets that nickelsync.cc constructs in place ---------------------

void TouchLabel::mouseReleaseEvent(QMouseEvent *event) {
    if (rect().contains(event->pos()))
        Q_EMIT tapped(false);
}

N3ButtonLabel::N3ButtonLabel(QWidget *parent) : TouchLabel(parent) {
    // The application style sheet has the button styles (nickelstub_mainWindow).
    setFont(uiFont(21, true));
}

N3Dialog::N3Dialog(QWidget *content) : QFrame(nullptr) {
    setObjectName(QStringLiteral("N3Dialog"));
    QVBoxLayout *l = new QVBoxLayout(this);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(0);
    QFrame *bar = new QFrame(this);
    bar->setObjectName(QStringLiteral("n3TitleBar"));
    QHBoxLayout *bl = new QHBoxLayout(bar);
    bl->setContentsMargins(24, 10, 12, 10);
    TouchLabel *backButton = new TouchLabel(bar);
    backButton->setObjectName(QStringLiteral("n3Back"));
    backButton->setPixmap(icon(QStringLiteral("<"), BAR_ICON));
    backButton->hide();
    back = backButton;
    title = new QLabel(bar);
    title->setFont(uiFont(24, true));
    TouchLabel *close = new TouchLabel(bar);
    close->setObjectName(QStringLiteral("n3Close"));
    close->setPixmap(icon(QStringLiteral("✕"), BAR_ICON));
    bl->addWidget(backButton);
    bl->addWidget(title, 1);
    bl->addWidget(close);
    l->addWidget(bar);
    if (content) {
        content->setParent(this);
        l->addWidget(content, 1);
    }
    connect(close, &TouchLabel::tapped, this, &N3Dialog::closeTapped);
    connect(backButton, &TouchLabel::tapped, this, &N3Dialog::backTapped);
}

// ---- the functions that nickelsync.cc resolves with nh_dlsym -----------

static QObject *N3SyncManager__sharedInstance() { return syncManager; }
static void N3SyncManager__silentSync(QObject *s) { static_cast<N3SyncManager *>(s)->silentSync(); }
static bool SyncManagerBase__isSyncing(QObject const *s) { return static_cast<N3SyncManager const *>(s)->syncing; }
static QObject *WirelessManager__sharedInstance() { return wirelessManager; }
static bool WirelessManager__connected(QObject *w) { return static_cast<WirelessManager *>(w)->connected; }
static QObject *WirelessWorkflowManager__sharedInstance() { return workflowManager; }
static void WirelessWorkflowManager__connectWirelessSilently(QObject *w) {
    static_cast<WirelessWorkflowManager *>(w)->connectWirelessSilently();
}
static bool WirelessWorkflowManager__isAirplaneMode(QObject *w) {
    return static_cast<WirelessWorkflowManager *>(w)->airplaneMode;
}
static void WirelessWorkflowManager__setAirplaneMode(QObject *w, bool on) {
    simLog(QStringLiteral("airplane mode: %1").arg(on ? "on" : "off"));
    static_cast<WirelessWorkflowManager *>(w)->airplaneMode = on;
    if (on)
        wirelessManager->connected = false;
}
static QObject *MainWindowController__sharedInstance() { return mainWindowController; }
static void MainWindowController__toast(QObject *m, QString const &main, QString const &sub, int ms) {
    static_cast<MainWindowController *>(m)->toast(main, sub, ms);
}
static QWidget *MainWindowController__viewWithObjectName(QObject const *, QString const &name) {
    return mainWindow ? mainWindow->findChild<QWidget *>(name) : nullptr;
}
static void const *ReadingView__getVolume(void const *) { return &volume; }
// Content is a base class of Volume. The sim has one open book.
static QString Content__getId(void const *) { return simOptions.bookId; }
static QString Content__getTitle(void const *) { return simOptions.title; }
static QString Content__getAttribution(void const *) { return simOptions.author; }

// nickelsync.cc allocates these objects with calloc and calls the Nickel
// constructor in place. The sizes are the sizes that it allocates.
static void TouchLabel__constructor(QLabel *mem, QWidget *parent, QFlags<Qt::WindowType>) {
    static_assert(sizeof(TouchLabel) <= 128, "TouchLabel does not fit");
    new (mem) TouchLabel(parent);
}
// The sim draws no hit state. It keeps the setting for the nohit: step.
static void TouchLabel__setHitStateEnabled(QLabel *l, bool on) { l->setProperty("simHitState", on); }
static void N3ButtonLabel__constructor(QLabel *mem, QWidget *parent) {
    static_assert(sizeof(N3ButtonLabel) <= 512, "N3ButtonLabel does not fit");
    new (mem) N3ButtonLabel(parent);
}
static QWidget *N3DialogFactory__getDialog(QWidget *content, bool) { return new N3Dialog(content); }
static void N3Dialog__setTitle(QWidget *d, QString const &text) { static_cast<N3Dialog *>(d)->title->setText(text); }

// pushView shows a view over the current one, as the view stack of Nickel
// does. The book hides while the view is open, and shows again when the view
// is deleted. Nickel connects to the view before the mod can, so the sim does
// too. The view gets the size of the Kobo window, not of the desktop screen.
static QWidget *MainWindowController__pushView(QObject *, QWidget *view) {
    simLog(QStringLiteral("MainWindowController::pushView: %1").arg(view->metaObject()->className()));
    if (N3Dialog *d = qobject_cast<N3Dialog *>(view))
        for (N3Dialog *under : mainWindow->findChildren<N3Dialog *>())
            if (under != d && under->isVisible())
                d->back->show();
    view->setParent(mainWindow);
    view->setFixedSize(mainWindow->size());
    view->move(0, 0);
    readingView->hide();
    QObject::connect(view, &QObject::destroyed, [] {
        simLog(QStringLiteral("view closed"));
        readingView->show();
    });
    return view;
}
static QObject *N3PowerWorkflowManager__sharedInstance() { return powerWorkflowManager; }
static bool N3PowerWorkflowManager__sleeping(QObject const *p) { return static_cast<N3PowerWorkflowManager const *>(p)->sleeping; }
static QObject *PowerManager__sharedInstance() { return powerManager; }
static void ScopedSuspendDisable__constructor(QObject *mem) {
    static_assert(sizeof(ScopedSuspendDisable) <= 64, "ScopedSuspendDisable does not fit");
    new (mem) ScopedSuspendDisable();
}

#define SYM(mangled, fn) {mangled, reinterpret_cast<void *>(fn)}
static const struct {
    const char *name;
    void *fn;
} symbols[] = {
    SYM("_ZN13N3SyncManager14sharedInstanceEv", N3SyncManager__sharedInstance),
    SYM("_ZN13N3SyncManager10silentSyncEv", N3SyncManager__silentSync),
    SYM("_ZNK15SyncManagerBase9isSyncingEv", SyncManagerBase__isSyncing),
    SYM("_ZN15WirelessManager14sharedInstanceEv", WirelessManager__sharedInstance),
    SYM("_ZN15WirelessManager9connectedEv", WirelessManager__connected),
    SYM("_ZN23WirelessWorkflowManager14sharedInstanceEv", WirelessWorkflowManager__sharedInstance),
    SYM("_ZN23WirelessWorkflowManager23connectWirelessSilentlyEv", WirelessWorkflowManager__connectWirelessSilently),
    SYM("_ZN23WirelessWorkflowManager14isAirplaneModeEv", WirelessWorkflowManager__isAirplaneMode),
    SYM("_ZN23WirelessWorkflowManager15setAirplaneModeEb", WirelessWorkflowManager__setAirplaneMode),
    SYM("_ZN20MainWindowController14sharedInstanceEv", MainWindowController__sharedInstance),
    SYM("_ZN20MainWindowController5toastERK7QStringS2_i", MainWindowController__toast),
    SYM("_ZNK20MainWindowController18viewWithObjectNameERK7QString", MainWindowController__viewWithObjectName),
    SYM("_ZNK11ReadingView9getVolumeEv", ReadingView__getVolume),
    SYM("_ZN10TouchLabelC1EP7QWidget6QFlagsIN2Qt10WindowTypeEE", TouchLabel__constructor),
    SYM("_ZN10TouchLabel18setHitStateEnabledEb", TouchLabel__setHitStateEnabled),
    SYM("_ZN13N3ButtonLabelC1EP7QWidget", N3ButtonLabel__constructor),
    SYM("_ZN15N3DialogFactory9getDialogEP7QWidgetb", N3DialogFactory__getDialog),
    SYM("_ZN8N3Dialog8setTitleERK7QString", N3Dialog__setTitle),
    SYM("_ZN20MainWindowController8pushViewEP7QWidget", MainWindowController__pushView),
    SYM("_ZN22N3PowerWorkflowManager14sharedInstanceEv", N3PowerWorkflowManager__sharedInstance),
    SYM("_ZNK22N3PowerWorkflowManager8sleepingEv", N3PowerWorkflowManager__sleeping),
    SYM("_ZN20ScopedSuspendDisableC1Ev", ScopedSuspendDisable__constructor),
    SYM("_ZN12PowerManager14sharedInstanceEv", PowerManager__sharedInstance),
    SYM("_ZNK7Content5getIdEv", Content__getId),
    SYM("_ZNK7Content8getTitleEv", Content__getTitle),
    SYM("_ZNK7Content14getAttributionEv", Content__getAttribution),
};

void *nickelstub_resolve(const char *name) {
    for (auto const &s : symbols)
        if (!std::strcmp(s.name, name))
            return s.fn;
    return nullptr;
}

QWidget *nickelstub_mainWindow() {
    if (!mainWindow) {
        syncManager = new N3SyncManager();
        wirelessManager = new WirelessManager();
        wirelessManager->connected = simOptions.wifi == SimOptions::WifiOn;
        workflowManager = new WirelessWorkflowManager();
        workflowManager->airplaneMode = simOptions.wifi != SimOptions::WifiOn;
        mainWindowController = new MainWindowController();
        powerWorkflowManager = new N3PowerWorkflowManager();
        powerManager = new PowerManager();
        // Nickel keeps its styles in the application style sheet. A page style
        // sheet overrides them. As on the device: a button without a style
        // property is italic text, and the darkMode property makes a widget
        // and its children white on black.
        qApp->setStyleSheet(QStringLiteral(
            "*[darkMode=true], *[darkMode=true] * { color: white; background-color: black; }"
            "N3Dialog { background: white; } #n3TitleBar { border-bottom: 2px solid black; }"
            "N3ButtonLabel { border: none; font-style: italic; padding: 12px 0px; }"
            "N3ButtonLabel[borderedButton=\"true\"] { border: 2px solid black; border-radius: 6px;"
            " padding: 12px 28px; font-style: normal; }"
            "N3ButtonLabel[primaryButton=\"true\"] { border: 2px solid black; border-radius: 6px;"
            " padding: 12px 28px; font-style: normal; background: black; color: white; }"));
        mainWindow = new QWidget();
        mainWindow->setObjectName(QStringLiteral("MainWindow"));
        mainWindow->setWindowTitle(QStringLiteral("NickelSync sim"));
        readingView = new ReadingView(mainWindow);
        QVBoxLayout *l = new QVBoxLayout(mainWindow);
        l->setContentsMargins(0, 0, 0, 0);
        l->addWidget(readingView);
        // ReadingView::showEvent sets darkMode on a child widget of the
        // reading view. The sim reading view has no child widget at the start,
        // so it gets an empty one.
        if (simOptions.dark)
            (new QWidget(readingView))->setProperty("darkMode", true);
    }
    return mainWindow;
}

ReadingView *nickelstub_readingView() { return readingView; }
N3PowerWorkflowManager *nickelstub_power() { return powerWorkflowManager; }

bool nickelstub_airplaneMode() { return workflowManager->airplaneMode; }
