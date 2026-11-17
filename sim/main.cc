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
//   --pick TEXT         the page button for --screenshot (default "Sync now")
//   --hardcover signed-out|signed-in|stale
//                       turn on the Hardcover section (Hardcover=true) and set
//                       the token store: no tokens, valid tokens, or an access
//                       token that the mock rejects, so NickelSync refreshes it.
//                       The mock server hcmock.py must run (run.sh starts it).
//   --isbn ISBN         the ISBN of the open book in the sim KoboReader.sqlite
//   --steps "A;B;..."   for --screenshot: after the NickelSync page opens, run
//                       these steps instead of a sync. A step is the text of a
//                       button or of a search result to tap, "wait:MS", or
//                       "expect:TEXT" / "absent:TEXT" to check the labels on
//                       the screen. A png follows each tap and wait.
//
// Exit codes in --screenshot mode: 0 pass, 3 no icon in the reading menu,
// 4 page button not found, 5 a NickelSync toast was not moved to the top,
// 6 no result toast within 90 s, 7 a step target was not found, 8 an expect or
// absent check failed.
#include <QApplication>
#include <QDir>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QDateTime>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
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

static void runScreenshot(QString const &pick) {
    QDir().mkpath(shotDir);
    QTimer::singleShot(300, [pick] {
        shot(QStringLiteral("page"));
        nickelstub_readingView()->showMenu();
        QTimer::singleShot(300, [pick] {
            shot(QStringLiteral("reading menu"));
            QLabel *icon = nickelstub_mainWindow()->findChild<QLabel *>(QStringLiteral("nickelsyncIcon"));
            if (!icon)
                return fail(3, "no nickelsyncIcon in the reading menu");
            QMetaObject::invokeMethod(icon, "tapped", Q_ARG(bool, false));
            QTimer::singleShot(300, [pick] {
                shot(QStringLiteral("nickelsync page"));
                for (N3ButtonLabel *b : nickelstub_mainWindow()->findChildren<N3ButtonLabel *>()) {
                    if (b->text() == pick) {
                        QMetaObject::invokeMethod(b, "tapped", Q_ARG(bool, false));
                        return;
                    }
                }
                fail(4, "page button not found");
            });
        });
    });
    QTimer::singleShot(90 * 1000, [] { fail(6, "no result toast within 90 s"); });
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
            return fail(8, want ? "expected text not shown" : "text shown that must be absent");
        }
        std::fprintf(stderr, "sim: step %s: ok\n", qPrintable(s));
        return runStep(steps, i + 1);
    }
    int wait = 1500;
    if (s.startsWith(QLatin1String("wait:"))) {
        wait = s.mid(5).toInt();
    } else {
        QLabel *t = tappable(s);
        if (!t) {
            std::fprintf(stderr, "sim: step %s\n", qPrintable(s));
            return fail(7, "step target not found");
        }
        QMetaObject::invokeMethod(t, "tapped", Q_ARG(bool, false));
    }
    QTimer::singleShot(wait, [steps, i, s] {
        shot(s.startsWith(QLatin1String("wait:")) ? QStringLiteral("wait") : QStringLiteral("tap ") + s);
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
            q.addBindValue(simOptions.contentId);
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
    bool quick = false;
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
        } else if (a == "--quick") {
            quick = true;
        } else if (a == "--screenshot") {
            shotDir = v;
            i++;
        } else if (a == "--pick") {
            pick = v;
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

    // The mod reads its config on each sync, so set QuickSync before the run.
    QDir().mkpath(QStringLiteral("/mnt/onboard/.adds/nickelsync"));
    {
        QSettings cfg(QStringLiteral("/mnt/onboard/.adds/nickelsync/config.ini"), QSettings::IniFormat);
        cfg.setValue(QStringLiteral("NickelSync/QuickSync"), quick);
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

    if (!shotDir.isEmpty() && !steps.isEmpty()) {
        runSteps(steps);
    } else if (!shotDir.isEmpty()) {
        app.installEventFilter(new ShotFilter());
        runScreenshot(pick);
    }
    w->show();
    int rc = app.exec();
    return exitCode ? exitCode : rc;
}
