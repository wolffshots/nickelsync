#ifndef NICKELSYNC_NICKEL_H
#define NICKELSYNC_NICKEL_H

// Nickel functions that more than one source file uses. The nh_dlsym table in
// nickelsync.cc fills them. Each one is optional except the sharedInstance
// functions, so check a pointer before a call.

#include <QFlags>
#include <QString>
#include <qnamespace.h>

class QLabel;
class QObject;
class QWidget;

extern QObject *(*MainWindowController__sharedInstance)();
extern QWidget *(*MainWindowController__viewWithObjectName)(QObject const *, QString const &);
extern QWidget *(*MainWindowController__pushView)(QObject *, QWidget *);
extern void const *(*ReadingView__getVolume)(void const *); // returns Volume const&
extern void (*TouchLabel__constructor)(QLabel *, QWidget *, QFlags<Qt::WindowType>);
extern void (*N3ButtonLabel__constructor)(QLabel *, QWidget *);
extern QWidget *(*N3DialogFactory__getDialog)(QWidget *, bool);
extern void (*N3Dialog__setTitle)(QWidget *, QString const &);
extern QObject *(*WirelessManager__sharedInstance)();
extern bool (*WirelessManager__connected)(QObject *);

// Content is a base class of Volume. Optional, for the server stats and the
// Hardcover page.
extern QString (*Content__getId)(void const *);
extern QString (*Content__getTitle)(void const *);
extern QString (*Content__getAttribution)(void const *);

// openSubPage shows page, an N3Dialog, over the NickelSync page. The back
// button closes page. The close button closes page, the other sub-pages and
// the NickelSync page. nickelsync.cc defines it.
void openSubPage(QWidget *page);

// connectPageWifi connects Wi-Fi for a page over the NickelSync page. When the
// NickelSync page closes, Wi-Fi goes off again if it was off before and the
// setting RestoreWifi is true. The caller waits for networkConnected() or
// networkFailedToConnect() of the WirelessManager. tag goes into the log.
// nickelsync.cc defines it.
void connectPageWifi(char const *tag);

// followDarkMode makes page, an N3Dialog, white on black when the open book
// uses dark mode. content is the widget of the page that NickelSync made.
// Call it before the page shows. nickelsync.cc defines it.
void followDarkMode(QWidget *page, QWidget *content);

#endif
