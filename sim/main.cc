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
//                       there is no config.ini (unless --quick, --auto or
//                       --hardcover).
//   --settings K1,K2    for --screenshot: open the settings page, tap the
//                       button of each key in turn, go back, check the
//                       Hardcover button, open the page again, then close all
//                       pages with the close button. run.sh checks config.ini.
//   --auto open|sleep|off
//                       with --screenshot: an auto-sync scenario instead of the
//                       taps. open: a book loads (AutoSyncOnOpen=true). sleep:
//                       the device goes to sleep (AutoSyncOnSleep=true). off:
//                       both events, with both keys false.
//   --no-retry          PowerManager does not try to suspend again after a veto
//   --expect K=N,...    with --auto: the counts to check at the end. Keys:
//                       pulls, pushes, syncs, toasts, vetoes, suspended (0 or 1).
//   --hardcover signed-out|signed-in|stale
//                       turn on the Hardcover page (Hardcover=true) and set
//                       the token store: no tokens, valid tokens, or an access
//                       token that the mock rejects, so NickelSync refreshes it.
//                       The mock server hcmock.py must run (run.sh starts it).
//   --isbn ISBN         the ISBN of the open book in the sim KoboReader.sqlite
//   --dark              the open book uses dark mode
//   --flush-log         log each paint pass of the Kobo window. With --steps,
//                       check the paint passes of each step (exit code 18).
//   --steps "A;B;..."   for --screenshot: after the NickelSync page opens, run
//                       these steps instead of a sync. A step is the text of a
//                       button or of a search result to tap, "wait:MS", or
//                       "expect:TEXT" / "absent:TEXT" to check the labels on
//                       the screen. "back" and "close" tap the back or close
//                       button of the top page. "closed" checks that no page
//                       is left. "nohit:TEXT" checks that the button has no
//                       hit state. "dark" checks that the top page is white
//                       on black. A png follows each tap and wait.
//
// Exit codes in --screenshot mode: 0 pass, 3 no icon in the reading menu,
// 4 page button not found, 5 a NickelSync toast was not moved to the top,
// 6 no result toast within 90 s, 7 setting button not found, 8 a value
// changed when the settings page opened again, 9 an --expect count differs,
// 10 the server stats state is not the expected state, 11 Wi-Fi is still on
// after the page closed, 12 a step target was not found, 13 an expect or
// absent check failed, 14 a visible button has no button style, 15 the close
// button left a NickelSync page open, 16 the Hardcover button does not match
// the Hardcover setting, 17 the back button did not close its page, 18 with
// --flush-log, a step made more than one large paint pass or a repeated pass,
// 19 a button that opens a page has the hit state (nohit: step), 20 a page is
// not white on black in dark mode (dark step).
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QLabel>
#include <QMap>
#include <QMenu>
#include <QPaintEvent>
#include <QPainter>
#include <QPointer>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTimer>

#include <cstdio>
#include <cstdlib>
#include <functional>

#include <NickelHook.h>

#include "nickelstub.h"

extern struct nh NickelHook;

static QString shotDir;
static int shotIndex;
static bool grabbing = false; // shot() renders the window, which is not a screen update
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

// FlushLog counts the paint passes of the Kobo window. Qt paints the widgets
// of one pass together and then flushes them to the screen. On the device,
// each flush is one e-ink update, so a pass that covers most of the screen is
// a flash. It logs one line for each pass: its bounding box and the share of
// the screen.
//
// For --steps, it also checks each step: at most one pass that covers more
// than half of the screen (a page change), and no pass that repeats the one
// before it (a widget that a layout showed one event loop pass late).
class FlushLog : public QObject {
public:
    int passes = 0;
    int stepLarge = 0;    // passes of this step that cover more than half of the screen
    bool repeat = false;  // a pass of this step repeated the one before it
    void resetStep() {
        stepLarge = 0;
        repeat = false;
        last = QRect();
    }
    bool eventFilter(QObject *obj, QEvent *event) override {
        if (event->type() != QEvent::Paint || grabbing || !obj->isWidgetType())
            return false;
        QWidget *w = static_cast<QWidget *>(obj);
        QWidget *win = nickelstub_mainWindow();
        if (w->window() != win)
            return false;
        QRect r = static_cast<QPaintEvent *>(event)->rect().translated(w->mapTo(win, QPoint(0, 0)));
        if (pending.isEmpty())
            QTimer::singleShot(0, [this] { endPass(); });
        pending = pending.united(r);
        return false;
    }

private:
    QRect pending;
    QRect last;
    void endPass() {
        QWidget *win = nickelstub_mainWindow();
        int share = 100 * pending.width() * pending.height() / qMax(1, win->width() * win->height());
        passes++;
        if (share > 50)
            stepLarge++;
        if (pending == last)
            repeat = true;
        last = pending;
        std::fprintf(stderr, "sim: paint pass %d: %dx%d at %d,%d, %d%% of the screen\n", passes, pending.width(),
                     pending.height(), pending.x(), pending.y(), share);
        pending = QRect();
    }
};
static FlushLog *flushLog = nullptr;

static void shot(QString const &name) {
    QString slug = name.toLower();
    slug.replace(QRegExp(QStringLiteral("[^a-z0-9]+")), QStringLiteral("-"));
    QString path = QStringLiteral("%1/%2-%3.png").arg(shotDir).arg(++shotIndex, 2, 10, QLatin1Char('0')).arg(slug);
    grabbing = true;
    composite().save(path);
    grabbing = false;
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

// stylesOk checks that each visible button has a Nickel button style. Without
// one, Nickel draws the button as italic text.
static bool stylesOk() {
    for (N3ButtonLabel *b : nickelstub_mainWindow()->findChildren<N3ButtonLabel *>()) {
        if (b->isVisible() && !b->property("primaryButton").toBool() && !b->property("borderedButton").toBool()) {
            std::fprintf(stderr, "sim: button without a style: %s\n", qPrintable(b->text()));
            return false;
        }
    }
    return true;
}

// topDialog is the N3Dialog on top: the last visible one.
static N3Dialog *topDialog() {
    N3Dialog *top = nullptr;
    for (N3Dialog *d : nickelstub_mainWindow()->findChildren<N3Dialog *>())
        if (d->isVisible())
            top = d;
    return top;
}

// checkAllClosed checks, a moment after a tap on the close button, that no
// NickelSync page is left.
static void checkAllClosed(std::function<void()> next) {
    QTimer::singleShot(300, [next] {
        shot(QStringLiteral("closed"));
        if (!nickelstub_mainWindow()->findChildren<N3Dialog *>().isEmpty())
            return fail(15, "the close button left a NickelSync page open");
        std::fprintf(stderr, "sim: all pages closed\n");
        next();
    });
}

// runSettings opens the settings page from the NickelSync page and taps the
// button of each key. It goes back with the back button, checks the Hardcover
// button, opens the page again and closes all pages with the close button.
static void runSettings(QStringList const &keys) {
    N3ButtonLabel *open = findButton(QStringLiteral("Settings"));
    if (!open)
        return fail(4, "Settings button not found");
    QMetaObject::invokeMethod(open, "tapped", Q_ARG(bool, false));
    QTimer::singleShot(300, [keys] {
        shot(QStringLiteral("settings"));
        if (!stylesOk())
            return fail(14, "a visible button has no button style");
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
            N3Dialog *d = topDialog();
            if (!d || d->title->text() != QLatin1String("NickelSync settings") || !d->back->isVisible())
                return fail(4, "no back button on the settings page");
            QMetaObject::invokeMethod(d, "backTapped");
            QPointer<N3Dialog> settingsPage = d;
            QTimer::singleShot(300, [before, settingsPage] {
                shot(QStringLiteral("back"));
                if (settingsPage)
                    return fail(17, "the back button did not close the settings page");
                N3ButtonLabel *again = findButton(QStringLiteral("Settings"));
                if (!again || !again->isVisible())
                    return fail(4, "Settings button not found after back");
                N3ButtonLabel *hc = findButton(QStringLiteral("Hardcover"));
                bool want = before.contains(QStringLiteral("Hardcover=On"));
                if (!hc || hc->isVisible() != want)
                    return fail(16, "the Hardcover button does not match the Hardcover setting");
                QMetaObject::invokeMethod(again, "tapped", Q_ARG(bool, false));
                QTimer::singleShot(300, [before] {
                    shot(QStringLiteral("settings reopened"));
                    if (settingTexts() != before)
                        return fail(8, "a value changed when the settings page opened again");
                    QMetaObject::invokeMethod(topDialog(), "closeTapped");
                    checkAllClosed([] { QApplication::quit(); });
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
                if (!stylesOk())
                    return fail(14, "a visible button has no button style");
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

// tappable finds the visible button or search result with this text, or with
// this first line. The last match is on top, as pages are added later.
static QLabel *tappable(QString const &text) {
    QLabel *found = nullptr;
    for (TouchLabel *t : nickelstub_mainWindow()->findChildren<TouchLabel *>())
        if (t->isVisible() && (t->text() == text || t->text().section(QLatin1Char('\n'), 0, 0) == text))
            found = t;
    return found;
}

static bool shown(QString const &text) {
    for (QLabel *l : nickelstub_mainWindow()->findChildren<QLabel *>())
        if (l->isVisible() && l->text().contains(text))
            return true;
    return false;
}

static void runStep(QStringList const &steps, int i) {
    if (i == steps.size()) {
        QTimer::singleShot(0, qApp, &QApplication::quit);
        return;
    }
    QString s = steps[i];
    if (s.startsWith(QLatin1String("expect:")) || s.startsWith(QLatin1String("absent:"))) {
        bool want = s.startsWith(QLatin1String("expect:"));
        if (shown(s.mid(7)) != want) {
            std::fprintf(stderr, "sim: step %s\n", qPrintable(s));
            return fail(13, want ? "expected text not shown" : "text shown that must be absent");
        }
        std::fprintf(stderr, "sim: step %s: ok\n", qPrintable(s));
        return runStep(steps, i + 1);
    }
    if (s.startsWith(QLatin1String("nohit:"))) {
        QLabel *t = tappable(s.mid(6));
        if (!t || t->property("simHitState") != QVariant(false)) {
            std::fprintf(stderr, "sim: step %s\n", qPrintable(s));
            return fail(19, "a button that opens a page has the hit state");
        }
        std::fprintf(stderr, "sim: step %s: ok\n", qPrintable(s));
        return runStep(steps, i + 1);
    }
    if (s == QLatin1String("dark")) {
        // The page background must be black. A bordered button must have
        // white text and a white border. No primary button may show: its
        // text is hard to see on black on the device.
        N3Dialog *d = topDialog();
        QImage img = d ? d->grab().toImage() : QImage();
        bool ok = !img.isNull() && qGray(img.pixel(img.width() / 2, img.height() - 5)) < 64;
        for (N3ButtonLabel *b : d ? d->findChildren<N3ButtonLabel *>() : QList<N3ButtonLabel *>()) {
            if (ok && b->isVisible() && b->property("primaryButton").toBool())
                ok = false;
            if (!ok || !b->isVisible() || b->property("borderedButton") != QVariant(true))
                continue;
            QPoint edge = b->mapTo(d, QPoint(b->width() / 2, 0));
            ok = qGray(img.pixel(edge)) > 192 && b->palette().color(QPalette::WindowText) == Qt::white;
        }
        if (!ok)
            return fail(20, "a page is not white on black in dark mode");
        std::fprintf(stderr, "sim: step dark: ok\n");
        return runStep(steps, i + 1);
    }
    if (s == QLatin1String("closed")) {
        if (!nickelstub_mainWindow()->findChildren<N3Dialog *>().isEmpty())
            return fail(15, "the close button left a NickelSync page open");
        std::fprintf(stderr, "sim: step closed: ok\n");
        return runStep(steps, i + 1);
    }
    int wait = 1500;
    QPointer<N3Dialog> backFrom;
    if (flushLog)
        flushLog->resetStep();
    if (s.startsWith(QLatin1String("wait:"))) {
        wait = s.mid(5).toInt();
    } else if (s == QLatin1String("back") || s == QLatin1String("close")) {
        std::fprintf(stderr, "sim: tap %s\n", qPrintable(s));
        N3Dialog *d = topDialog();
        if (!d || (s == QLatin1String("back") && !d->back->isVisible())) {
            std::fprintf(stderr, "sim: step %s\n", qPrintable(s));
            return fail(12, "step target not found");
        }
        QMetaObject::invokeMethod(d, s == QLatin1String("back") ? "backTapped" : "closeTapped");
        if (s == QLatin1String("back"))
            backFrom = d;
        wait = 500;
    } else {
        QLabel *t = tappable(s);
        if (!t) {
            std::fprintf(stderr, "sim: step %s\n", qPrintable(s));
            return fail(12, "step target not found");
        }
        std::fprintf(stderr, "sim: tap %s\n", qPrintable(s));
        QMetaObject::invokeMethod(t, "tapped", Q_ARG(bool, false));
    }
    QTimer::singleShot(wait, [steps, i, s, backFrom] {
        shot(s.startsWith(QLatin1String("wait:")) ? QStringLiteral("wait") : QStringLiteral("tap ") + s);
        if (backFrom)
            return fail(17, "the back button did not close its page");
        if (flushLog && (flushLog->stepLarge > 1 || flushLog->repeat))
            return fail(18, "a step painted the screen more than once");
        if (!stylesOk())
            return fail(14, "a visible button has no button style");
        runStep(steps, i + 1);
    });
}

// runSteps opens the NickelSync page, then runs the steps.
static void runSteps(QStringList const &steps) {
    QDir().mkpath(shotDir);
    QTimer::singleShot(300, [steps] {
        nickelstub_readingView()->showMenu();
        QTimer::singleShot(300, [steps] {
            QLabel *icon = nickelstub_mainWindow()->findChild<QLabel *>(QStringLiteral("nickelsyncIcon"));
            if (!icon)
                return fail(3, "no nickelsyncIcon in the reading menu");
            QMetaObject::invokeMethod(icon, "tapped", Q_ARG(bool, false));
            QTimer::singleShot(500, [steps] {
                shot(QStringLiteral("nickelsync page"));
                runStep(steps, 0);
            });
        });
    });
    QTimer::singleShot(90 * 1000, [] { fail(6, "the steps did not end within 90 s"); });
}

// setupHardcover writes the config key, the token store and the sim database.
static void setupHardcover(QString const &state, QString const &isbn) {
    QSettings cfg(QStringLiteral("/mnt/onboard/.adds/nickelsync/config.ini"), QSettings::IniFormat);
    cfg.setValue(QStringLiteral("NickelSync/Hardcover"), true);
    if (state != QLatin1String("signed-out")) {
        bool stale = state == QLatin1String("stale");
        QSettings store(QStringLiteral("/mnt/onboard/.adds/nickelsync/hardcover.ini"), QSettings::IniFormat);
        store.setValue(QStringLiteral("auth/access_token"), stale ? "hc_at_old" : "hc_at_sim");
        store.setValue(QStringLiteral("auth/refresh_token"), stale ? "hc_rt_old" : "hc_rt_sim");
        store.setValue(QStringLiteral("auth/expires_at"), QDateTime::currentDateTimeUtc().addDays(7));
    }
    QDir().mkpath(QStringLiteral("/mnt/onboard/.kobo"));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("sim"));
        db.setDatabaseName(QStringLiteral("/mnt/onboard/.kobo/KoboReader.sqlite"));
        if (db.open()) {
            QSqlQuery q(db);
            q.exec(QStringLiteral("CREATE TABLE IF NOT EXISTS content (ContentID TEXT, ContentType INTEGER, ISBN TEXT)"));
            q.prepare(QStringLiteral("INSERT INTO content VALUES (?, 6, ?)"));
            q.addBindValue(simOptions.bookId);
            q.addBindValue(isbn);
            q.exec();
        }
    }
    QSqlDatabase::removeDatabase(QStringLiteral("sim"));
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
    QString hardcover, isbn;
    QStringList steps;
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
        } else if (a == "--flush-log") {
            flushLog = new FlushLog();
            app.installEventFilter(flushLog);
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
        } else if (a == "--dark") {
            simOptions.dark = true;
        } else if (a == "--no-retry") {
            simOptions.suspendRetry = false;
        } else if (a == "--expect") {
            for (QString const &kv : v.split(',', QString::SkipEmptyParts))
                expect[kv.section('=', 0, 0)] = kv.section('=', 1).toInt();
            i++;
        } else if (a == "--hardcover") {
            hardcover = v;
            i++;
        } else if (a == "--isbn") {
            isbn = v;
            i++;
        } else if (a == "--steps") {
            steps = v.split(';', QString::SkipEmptyParts);
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
    if (!hardcover.isEmpty())
        setupHardcover(hardcover, isbn);

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
    } else if (!shotDir.isEmpty() && !steps.isEmpty()) {
        runSteps(steps);
    } else if (!shotDir.isEmpty()) {
        app.installEventFilter(new ShotFilter());
        runScreenshot(pick, settings, expectStats);
    }
    w->show();
    int rc = app.exec();
    return exitCode ? exitCode : rc;
}
