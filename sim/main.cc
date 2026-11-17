// NickelSync desktop sim: runs src/nickelsync.cc unmodified against the Nickel
// stand-ins in nickelstub.cc.
//
//   --size WxH          screen size (default 758x1008, the Libra Colour shape)
//   --wifi off|on|fail  Wi-Fi at the start (default off, so NickelSync connects)
//   --server same|newer|none
//                       the server reading position: same (no prompt), newer
//                       (Nickel shows "Sync to last page read?") or none (no
//                       answer, as for a sideloaded book; the pull waits 20 s)
//   --sync-ms N         duration of the fake full sync (default 3000)
//   --quick             write QuickSync=true to the NickelSync config
//   --bar-icons A,B     icons of other mods in the reading menu bar, added as
//                       NickelHardcover adds its icon. "hardcover" is the
//                       NickelHardcover icon. Other names get a placeholder.
//   --screenshot DIR    open the reading menu, tap the icon, tap a button on
//                       the NickelSync page, save a png for each step and
//                       toast, then exit (run.sh uses the VNC platform)
//   --pick TEXT         the page button for --screenshot (default "Sync now").
//                       "none" taps no button: the run waits for the server
//                       stats, closes the page and checks that Wi-Fi is off
//                       again if it was off at the start (--wifi off or fail).
//   --stats ok|slow|error|missing|timeout|store
//                       the answer of the fake Calibre-Web to the server stats
//                       request (fakeserver.cc): ok after 0.4 s, slow (ok after
//                       6 s), error (HTTP 500), missing (HTTP 404), timeout (no
//                       answer, NickelSync waits 15 s), store (the sync server
//                       is the Kobo store, so NickelSync sends nothing)
//   --book file         the open book is sideloaded (a file:// ContentID)
//   --expect-stats S    with --pick none: the server stats state to expect,
//                       done, error or none (default done)
//   --config FILE       copy FILE to config.ini before the run. Without it,
//                       there is no config.ini (unless --quick or --auto).
//   --settings K1,K2    for --screenshot: open the settings page, tap the
//                       button of each key in turn, close the page, open it
//                       again, then exit. run.sh checks config.ini.
//   --auto open|sleep|off
//                       with --screenshot: an auto-sync scenario instead of the
//                       taps. open: a book loads (AutoSyncOnOpen=true). sleep:
//                       the device goes to sleep (AutoSyncOnSleep=true). off:
//                       both events, with both keys false.
//   --no-retry          PowerManager does not try to suspend again after a veto
//   --expect K=N,...    with --auto: the counts to check at the end. Keys:
//                       pulls, pushes, syncs, toasts, vetoes, suspended (0 or 1).
//
// Exit codes in --screenshot mode: 0 pass, 3 no icon in the reading menu,
// 4 page button not found, 5 a NickelSync toast was not moved to the top,
// 6 no result toast within 90 s, 7 setting button not found, 8 a value
// changed when the settings page opened again, 9 an --expect count differs,
// 10 the server stats state is not the expected state, 11 Wi-Fi is still on
// after the page closed.
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QMap>
#include <QMenu>
#include <QPainter>
#include <QSettings>
#include <QTimer>

#include <cstdio>
#include <cstdlib>

#include <NickelHook.h>

#include "nickelstub.h"

extern struct nh NickelHook;

static QString shotDir;
static int shotIndex;
static int exitCode = 0;

// composite grabs the screen window with any top-level window on top, for
// example a popup menu. A grab of the screen window alone does not show it.
static QPixmap composite() {
    QWidget *w = nickelstub_mainWindow();
    QPixmap pm = w->grab();
    QPainter p(&pm);
    for (QWidget *t : QApplication::topLevelWidgets())
        if (t != w && t->isVisible())
            p.drawPixmap(w->mapFromGlobal(t->pos()), t->grab());
    return pm;
}

static void shot(QString const &name) {
    QString slug = name.toLower();
    slug.replace(QRegExp(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    QString path = QStringLiteral("%1/%2-%3.png").arg(shotDir).arg(++shotIndex, 2, 10, QLatin1Char('0')).arg(slug);
    composite().save(path);
    std::fprintf(stderr, "sim: wrote %s\n", qPrintable(path));
}

static void fail(int code, const char *why) {
    std::fprintf(stderr, "sim: FAIL: %s\n", why);
    exitCode = code;
    QTimer::singleShot(0, qApp, &QApplication::quit);
}

// ShotFilter saves a png shortly after each toast or prompt shows, so that
// NickelSync has moved the toast first. It also checks the toast position.
class ShotFilter : public QObject {
public:
    bool eventFilter(QObject *obj, QEvent *event) override {
        if (event->type() != QEvent::Show)
            return false;
        if (obj->inherits("ConfirmationDialog")) {
            QTimer::singleShot(100, [] { shot(QStringLiteral("prompt")); });
        } else if (obj->inherits("N3ToastView")) {
            QWidget *t = static_cast<QWidget *>(obj);
            QLabel *l = t->findChild<QLabel *>(QStringLiteral("primaryText"));
            QString text = l ? l->text() : QString();
            QTimer::singleShot(100, [t, text] {
                shot(QStringLiteral("toast ") + text);
                if (t->y() > t->parentWidget()->height() / 4)
                    fail(5, "a NickelSync toast is not at the top");
            });
            // The first two toasts are progress. Any other toast is the result.
            if (text != QLatin1String("Connecting to Wi-Fi") && text != QLatin1String("Syncing"))
                QTimer::singleShot(4000, qApp, &QApplication::quit);
        }
        return false;
    }
};

static N3ButtonLabel *findButton(QString const &text) {
    for (N3ButtonLabel *b : nickelstub_mainWindow()->findChildren<N3ButtonLabel *>())
        if (b->text() == text)
            return b;
    return nullptr;
}

// settingTexts gives the text of each setting button, in page order.
static QStringList settingTexts() {
    QStringList out;
    for (N3ButtonLabel *b : nickelstub_mainWindow()->findChildren<N3ButtonLabel *>())
        if (b->property("nsKey").isValid())
            out << QString::fromLatin1(b->property("nsKey").toByteArray()) + "=" + b->text();
    return out;
}

// runSettings opens the settings page from the NickelSync page, taps the
// button of each key, closes the page and opens it again.
static void runSettings(QStringList const &keys) {
    N3ButtonLabel *open = findButton(QStringLiteral("Settings"));
    if (!open)
        return fail(4, "Settings button not found");
    QMetaObject::invokeMethod(open, "tapped", Q_ARG(bool, false));
    QTimer::singleShot(300, [keys] {
        shot(QStringLiteral("settings"));
        for (QString const &key : keys) {
            N3ButtonLabel *b = nullptr;
            for (N3ButtonLabel *c : nickelstub_mainWindow()->findChildren<N3ButtonLabel *>())
                if (c->property("nsKey").toByteArray() == key.toLatin1())
                    b = c;
            if (!b)
                return fail(7, "setting button not found");
            QMetaObject::invokeMethod(b, "tapped", Q_ARG(bool, false));
        }
        QStringList before = settingTexts();
        std::fprintf(stderr, "sim: settings: %s\n", qPrintable(before.join(' ')));
        QTimer::singleShot(300, [before] {
            shot(QStringLiteral("settings changed"));
            for (N3Dialog *d : nickelstub_mainWindow()->findChildren<N3Dialog *>())
                if (d->title->text() == QLatin1String("NickelSync settings"))
                    QMetaObject::invokeMethod(d, "closeTapped");
            QTimer::singleShot(300, [before] {
                N3ButtonLabel *again = findButton(QStringLiteral("Settings"));
                if (!again)
                    return fail(4, "Settings button not found after close");
                QMetaObject::invokeMethod(again, "tapped", Q_ARG(bool, false));
                QTimer::singleShot(300, [before] {
                    shot(QStringLiteral("settings reopened"));
                    if (settingTexts() != before)
                        return fail(8, "a value changed when the settings page opened again");
                    QApplication::quit();
                });
            });
        });
    });
}

// waitStats waits for the server stats to load, closes the page and checks
// the Wi-Fi state. It is the end of a --pick none run.
static void waitStats(QString const &expect) {
    QWidget *stats = nickelstub_mainWindow()->findChild<QWidget *>(QStringLiteral("nsServerStats"));
    QString state = stats ? stats->property("nsStatsState").toString() : QStringLiteral("missing");
    if (state == QLatin1String("loading")) {
        QTimer::singleShot(200, [expect] { waitStats(expect); });
        return;
    }
    std::fprintf(stderr, "sim: server stats state: %s\n", qPrintable(state));
    shot(QStringLiteral("server stats ") + state);
    if (state != expect)
        return fail(10, "the server stats state is not the expected state");
    QWidget *page = nickelstub_mainWindow()->findChild<QWidget *>(QStringLiteral("N3Dialog"));
    if (page)
        QMetaObject::invokeMethod(page, "closeTapped");
    QTimer::singleShot(1000, [] {
        shot(QStringLiteral("closed"));
        if (simOptions.wifi != SimOptions::WifiOn && !nickelstub_airplaneMode())
            return fail(11, "Wi-Fi is still on after the page closed");
        QApplication::quit();
    });
}

static void runScreenshot(QString const &pick, QStringList const &settings, QString const &expectStats) {
    QDir().mkpath(shotDir);
    QTimer::singleShot(300, [pick, settings, expectStats] {
        shot(QStringLiteral("page"));
        nickelstub_readingView()->showMenu();
        QTimer::singleShot(300, [pick, settings, expectStats] {
            shot(QStringLiteral("reading menu"));
            QLabel *icon = nickelstub_mainWindow()->findChild<QLabel *>(QStringLiteral("nickelsyncIcon"));
            if (!icon)
                return fail(3, "no nickelsyncIcon in the reading menu");
            QMetaObject::invokeMethod(icon, "tapped", Q_ARG(bool, false));
            QTimer::singleShot(300, [pick, settings, expectStats] {
                shot(QStringLiteral("nickelsync page"));
                if (!settings.isEmpty())
                    return runSettings(settings);
                if (pick == QLatin1String("none"))
                    return waitStats(expectStats);
                if (N3ButtonLabel *b = findButton(pick))
                    QMetaObject::invokeMethod(b, "tapped", Q_ARG(bool, false));
                else
                    fail(4, "page button not found");
            });
        });
    });
    QTimer::singleShot(90 * 1000, [] { fail(6, "no result toast within 90 s"); });
}

// counts returns the SimStats values that --expect can check.
static QMap<QString, int> counts() {
    QMap<QString, int> c;
    c["pulls"] = simStats.pulls;
    c["pushes"] = simStats.pushes;
    c["syncs"] = simStats.syncs;
    c["toasts"] = simStats.toasts;
    c["suspended"] = simStats.suspended;
    c["vetoes"] = simStats.vetoes;
    return c;
}

// runAuto plays an auto-sync scenario. It ends when the counts match
// --expect, Wi-Fi is back as it was, and the scenario has run for 4 s, or
// after 85 s. Then it compares the counts.
static void runAuto(QString const &kind, QMap<QString, int> const &expect) {
    QDir().mkpath(shotDir);
    bool airplane = nickelstub_airplaneMode();
    QTimer::singleShot(300, [kind] {
        shot(QStringLiteral("book"));
        if (kind != QLatin1String("sleep"))
            Q_EMIT nickelstub_readingView()->readerDoneLoading();
    });
    if (kind != QLatin1String("open"))
        QTimer::singleShot(600, [] { nickelstub_power()->sleep(); });
    QElapsedTimer *clock = new QElapsedTimer();
    clock->start();
    QTimer *poll = new QTimer();
    QObject::connect(poll, &QTimer::timeout, [=] {
        QMap<QString, int> now = counts();
        bool match = true;
        for (auto it = expect.begin(); it != expect.end(); ++it)
            match = match && now.value(it.key()) == it.value();
        bool settled = clock->elapsed() > 4000 && match && nickelstub_airplaneMode() == airplane;
        if (!settled && clock->elapsed() < 85 * 1000)
            return;
        poll->stop();
        shot(QStringLiteral("end"));
        QStringList got;
        for (auto it = now.begin(); it != now.end(); ++it)
            got << QStringLiteral("%1=%2").arg(it.key()).arg(it.value());
        std::fprintf(stderr, "sim: counts %s\n", qPrintable(got.join(',')));
        if (!match)
            fail(9, "an --expect count differs");
        else
            QTimer::singleShot(0, qApp, &QApplication::quit);
    });
    poll->start(250);
}

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QStringList args = app.arguments();
    QSize size(758, 1008);
    QString pick = QStringLiteral("Sync now");
    QString expectStats = QStringLiteral("done");
    bool quick = false;
    QString config;
    QStringList settings;
    QString autoKind;
    QMap<QString, int> expect;
    for (int i = 1; i < args.size(); i++) {
        QString a = args[i];
        QString v = i + 1 < args.size() ? args[i + 1] : QString();
        if (a == "--size") {
            QStringList wh = v.split('x');
            size = QSize(wh.value(0).toInt(), wh.value(1).toInt());
            i++;
        } else if (a == "--wifi") {
            simOptions.wifi = v == "on" ? SimOptions::WifiOn : v == "fail" ? SimOptions::WifiFail : SimOptions::WifiOff;
            i++;
        } else if (a == "--server") {
            simOptions.server = v == "newer" ? SimOptions::ServerNewer
                              : v == "none"  ? SimOptions::ServerNone
                                             : SimOptions::ServerSame;
            i++;
        } else if (a == "--sync-ms") {
            simOptions.syncMs = v.toInt();
            i++;
        } else if (a == "--bar-icons") {
            simOptions.barIcons = v.split(',', QString::SkipEmptyParts);
            i++;
        } else if (a == "--stats") {
            QStringList modes = {"ok", "slow", "error", "missing", "timeout", "store"};
            if (!modes.contains(v)) {
                std::fprintf(stderr, "sim: unknown --stats %s\n", qPrintable(v));
                return 2;
            }
            simOptions.stats = SimOptions::Stats(modes.indexOf(v));
            i++;
        } else if (a == "--book") {
            if (v == "file")
                simOptions.bookId = QStringLiteral("file:///mnt/onboard/books/lighthouse.kepub.epub");
            i++;
        } else if (a == "--expect-stats") {
            expectStats = v;
            i++;
        } else if (a == "--quick") {
            quick = true;
        } else if (a == "--screenshot") {
            shotDir = v;
            i++;
        } else if (a == "--pick") {
            pick = v;
            i++;
        } else if (a == "--config") {
            config = v;
            i++;
        } else if (a == "--settings") {
            settings = v.split(',', QString::SkipEmptyParts);
            i++;
        } else if (a == "--auto") {
            autoKind = v;
            i++;
        } else if (a == "--no-retry") {
            simOptions.suspendRetry = false;
        } else if (a == "--expect") {
            for (QString const &kv : v.split(',', QString::SkipEmptyParts))
                expect[kv.section('=', 0, 0)] = kv.section('=', 1).toInt();
            i++;
        } else {
            std::fprintf(stderr, "sim: unknown option %s\n", qPrintable(a));
            return 2;
        }
    }

    // The mod reads its config on each sync, so write it before the run.
    QDir().mkpath(QStringLiteral("/mnt/onboard/.adds/nickelsync"));
    QString cfgPath = QStringLiteral("/mnt/onboard/.adds/nickelsync/config.ini");
    if (!config.isEmpty() && !QFile::copy(config, cfgPath)) {
        std::fprintf(stderr, "sim: could not copy %s to %s\n", qPrintable(config), qPrintable(cfgPath));
        return 2;
    }
    if (quick || !autoKind.isEmpty()) {
        QSettings cfg(cfgPath, QSettings::IniFormat);
        if (quick)
            cfg.setValue(QStringLiteral("NickelSync/QuickSync"), true);
        if (!autoKind.isEmpty()) {
            cfg.setValue(QStringLiteral("NickelSync/AutoSyncOnOpen"), autoKind == QLatin1String("open"));
            cfg.setValue(QStringLiteral("NickelSync/AutoSyncOnSleep"), autoKind == QLatin1String("sleep"));
        }
    }
    // The sync server of Nickel. The server stats read it.
    {
        QDir().mkpath(QStringLiteral("/mnt/onboard/.kobo/Kobo"));
        QSettings conf(QStringLiteral("/mnt/onboard/.kobo/Kobo/Kobo eReader.conf"), QSettings::IniFormat);
        conf.setValue(QStringLiteral("OneStoreServices/api_endpoint"), fakeserver_start());
    }

    QWidget *w = nickelstub_mainWindow();
    w->setFixedSize(size);
    w->move(0, 0); // in --screenshot mode the window is the whole screen, as on the device

    // What NickelHook does on the device: resolve the dlsym table, then call
    // the init function of the mod.
    for (struct nh_dlsym *d = NickelHook.dlsym; d->name; d++) {
        *d->out = nickelstub_resolve(d->name);
        if (!*d->out && !d->optional) {
            std::fprintf(stderr, "sim: no stand-in for required symbol %s\n", d->name);
            return 2;
        }
    }
    if (NickelHook.init && NickelHook.init() != 0) {
        std::fprintf(stderr, "sim: the mod init failed\n");
        return 2;
    }

    if (!shotDir.isEmpty() && !autoKind.isEmpty()) {
        runAuto(autoKind, expect);
    } else if (!shotDir.isEmpty()) {
        app.installEventFilter(new ShotFilter());
        runScreenshot(pick, settings, expectStats);
    }
    w->show();
    int rc = app.exec();
    return exitCode ? exitCode : rc;
}
