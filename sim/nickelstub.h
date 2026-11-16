// Desktop stand-ins for the Nickel classes that src/nickelsync.cc touches.
//
// nickelsync.cc finds Nickel objects by class name (inherits(), qt_metacast())
// and by object name, and it connects Nickel signals by their string
// signatures. So each stand-in has the Nickel class name, and the same
// signals, slots and object names. The stand-ins draw plain Qt widgets that
// look roughly like the Kobo UI. They are not Nickel and do not match it pixel
// for pixel.
#ifndef NICKELSTUB_H
#define NICKELSTUB_H

#include <QFrame>
#include <QLabel>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QWidget>

class QHBoxLayout;

struct Volume {
    int unused;
};

namespace SyncState {
enum SyncType { Full };
}

// SimOptions are the command line options of the sim (see main.cc).
struct SimOptions {
    enum Wifi { WifiOff, WifiOn, WifiFail };
    enum Server { ServerSame, ServerNewer, ServerNone };
    Wifi wifi = WifiOff;
    Server server = ServerSame;
    int connectMs = 2000;
    int pullMs = 1000;
    int syncMs = 3000;
    QStringList barIcons; // other mods with an icon in the reading menu bar
};
extern SimOptions simOptions;

// nickelstub_resolve returns the stand-in for a mangled Nickel symbol, or null.
void *nickelstub_resolve(const char *name);
// nickelstub_mainWindow returns the Kobo screen window. It creates it on the
// first call.
QWidget *nickelstub_mainWindow();
class ReadingView *nickelstub_readingView();

// ---- sync and Wi-Fi ----------------------------------------------------

class N3SyncManager : public QObject {
    Q_OBJECT
public:
    void silentSync();
    bool syncing = false;
Q_SIGNALS:
    void syncStarted(SyncState::SyncType type);
    void finished();
    void downloadStarted(Volume volume);
    void bookmarkRequestSuccess(Volume volume);
};

class WirelessManager : public QObject {
    Q_OBJECT
public:
    bool connected = false;
Q_SIGNALS:
    void networkConnected();
    void networkFailedToConnect();
};

class WirelessWorkflowManager : public QObject {
    Q_OBJECT
public:
    void connectWirelessSilently();
    bool airplaneMode = true;
};

// ---- main window and toasts --------------------------------------------

class N3ToastView : public QFrame {
    Q_OBJECT
public:
    N3ToastView(QWidget *parent, QString const &main, QString const &sub);
};

class MainWindowController : public QObject {
    Q_OBJECT
public:
    void toast(QString const &main, QString const &sub, int ms);
private:
    void showNext();
    QList<QPair<QPair<QString, QString>, int>> queue;
    bool showing = false;
};

class ConfirmationDialog : public QFrame {
    Q_OBJECT
public:
    ConfirmationDialog(QWidget *parent, QString const &title, QString const &text);
};

// ---- reader --------------------------------------------------------------

class KepubBookReaderBase : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;
public Q_SLOTS:
    void onResume();
};

class ReadingMenuView : public QWidget {
    Q_OBJECT
public:
    explicit ReadingMenuView(QWidget *parent);
protected:
    void mouseReleaseEvent(QMouseEvent *event) override;
};

class ReadingView : public QWidget {
    Q_OBJECT
public:
    explicit ReadingView(QWidget *parent);
    void showMenu();
    ReadingMenuView *menu = nullptr;
Q_SIGNALS:
    void saveBookmark();
    void sendBookmark(Volume volume);
protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
};

// ---- widgets that nickelsync.cc constructs in place ---------------------

class TouchLabel : public QLabel {
    Q_OBJECT
public:
    explicit TouchLabel(QWidget *parent) : QLabel(parent) {}
Q_SIGNALS:
    void tapped(bool);
protected:
    void mouseReleaseEvent(QMouseEvent *event) override;
};

// N3ButtonLabel is the Nickel button. primaryButton=true gives the dark style.
class N3ButtonLabel : public TouchLabel {
    Q_OBJECT
public:
    explicit N3ButtonLabel(QWidget *parent);
};

// N3Dialog is the full-screen Nickel page with a title bar and a close button.
class N3Dialog : public QFrame {
    Q_OBJECT
public:
    explicit N3Dialog(QWidget *content);
    QLabel *title;
Q_SIGNALS:
    void closeTapped();
};

#endif
