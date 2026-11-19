#ifndef SERVERSTATS_H
#define SERVERSTATS_H

#include <QPointer>
#include <QTimer>
#include <QWidget>

class QLabel;
class QNetworkReply;
class QVBoxLayout;

// ServerStats is the read-only view of the reading state that the sync server
// has for the open book: the position, the reading time and the raw location.
// It sends GET <api_endpoint>/v1/library/<ContentID>/state, the request that
// Nickel sends for the book. The endpoint is the api_endpoint that Nickel uses
// for its sync, so the view needs no credentials of its own. The request has no
// X-Kobo-DeviceId header, so Calibre-Web does not change any device state.
//
// The view shows a message at once. load() starts the request and returns.
// The answer or an error replaces the message later.
class ServerStats : public QWidget {
    Q_OBJECT

public:
    // bookId is the ContentID of the open book. spacing is the space between
    // the rows, in pixels.
    ServerStats(QString const &bookId, int spacing, QWidget *parent = nullptr);
    // needsNetwork is false if the view cannot load anything, for example for
    // a sideloaded book. The view then shows the reason.
    bool needsNetwork() const { return ready; }
    // waitForWifi starts load() when wirelessManager emits networkConnected().
    void waitForWifi(QObject *wirelessManager, int timeoutMs);

public Q_SLOTS:
    void load();

private Q_SLOTS:
    void onWifiFailed();
    void onReply();
    void onTimeout();

private:
    void showMessage(QString const &text, char const *state);
    void showStats(QByteArray const &body);
    void addRow(QString const &key, QString const &value);

    QString bookId;
    QString endpoint;
    bool ready = false;
    bool started = false;
    QVBoxLayout *rows;
    QLabel *message;
    QTimer timeout;
    QPointer<QNetworkReply> reply;
};

#endif
