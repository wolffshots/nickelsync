#ifndef NICKELSYNC_SETTINGS_H
#define NICKELSYNC_SETTINGS_H

#include <QObject>
#include <QPointer>

class QBoxLayout;
class QLabel;
class QWidget;

// settingBool and settingInt read one key of the [NickelSync] section of
// config.ini. The key must be in the settings table in settings.cc. A missing
// file, a missing key, a value that is not valid, and a file that does not
// parse give the default of the key. An integer outside its range is clamped.
bool settingBool(char const *key);
int settingInt(char const *key);

// SettingsPage is the NickelSync settings page. A "Settings" button on the
// NickelSync page opens it. Each setting has a button that shows its value.
// A tap sets the next value and writes config.ini at once.
class SettingsPage : public QObject {
    Q_OBJECT

public:
    // addButton adds the "Settings" button to the NickelSync page and returns it.
    static QLabel *addButton(QBoxLayout *layout, QWidget *parent);

private Q_SLOTS:
    void open();
    void onTap();

private:
    void refresh(QString const &message = QString());

    QPointer<QWidget> page;
    QPointer<QLabel> note;
};

#endif
