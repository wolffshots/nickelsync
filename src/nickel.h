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

#endif
