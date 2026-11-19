// ServerStats: the server stats of the open book on the NickelSync page. See
// serverstats.h.
//
// The request passes CCP in front of Calibre-Web: the path starts with /kobo,
// and the User-Agent has "Kobo" in it.

#include <QDateTime>
#include <QStringList>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QUrl>
#include <QVBoxLayout>

#include <NickelHook.h>

#include "serverstats.h"

#define NICKEL_CONF "/mnt/onboard/.kobo/Kobo/Kobo eReader.conf"

// Time for the server to answer. Nickel uses the same server for its sync.
static const int STATS_TIMEOUT_MS = 15 * 1000;
// CCP lets through only some user agents. The live list has "*Kobo*".
static char const STATS_AGENT[] = "NickelSync (Kobo)";

ServerStats::ServerStats(QString const &bookId, int spacing, QWidget *parent) : QWidget(parent), bookId(bookId) {
    setObjectName(QStringLiteral("nsServerStats"));
    rows = new QVBoxLayout(this);
    rows->setContentsMargins(0, 0, 0, 0);
    rows->setSpacing(spacing);
    QLabel *heading = new QLabel(QStringLiteral("On the server"));
    heading->setObjectName(QStringLiteral("nsHeading"));
    rows->addWidget(heading);
    message = new QLabel();
    message->setObjectName(QStringLiteral("nsDesc"));
    message->setWordWrap(true);
    rows->addWidget(message);
    timeout.setSingleShot(true);
    connect(&timeout, SIGNAL(timeout()), this, SLOT(onTimeout()));

    // The api_endpoint of Calibre-Web is https://<server>/kobo/<token>. Never
    // log it: the token is a credential.
    endpoint = QSettings(QStringLiteral(NICKEL_CONF), QSettings::IniFormat)
                   .value(QStringLiteral("OneStoreServices/api_endpoint"))
                   .toString();
    while (endpoint.endsWith('/'))
        endpoint.chop(1);
    if (bookId.isEmpty())
        showMessage(QStringLiteral("No open book."), "none");
    else if (bookId.startsWith(QLatin1String("file://")))
        showMessage(QStringLiteral("This book is not synced with the server."), "none");
    else if (!endpoint.contains(QLatin1String("/kobo/")))
        showMessage(QStringLiteral("Server stats need a Calibre-Web sync server."), "none");
    else {
        ready = true;
        showMessage(QStringLiteral("Loading the server stats"), "loading");
    }
}

void ServerStats::showMessage(QString const &text, char const *state) {
    message->setText(text);
    message->show();
    // The sim reads this property to check the state.
    setProperty("nsStatsState", QString::fromLatin1(state));
}

void ServerStats::waitForWifi(QObject *wirelessManager, int timeoutMs) {
    if (!ready || started)
        return;
    showMessage(QStringLiteral("Connecting to Wi-Fi"), "loading");
    connect(wirelessManager, SIGNAL(networkConnected()), this, SLOT(load()));
    connect(wirelessManager, SIGNAL(networkFailedToConnect()), this, SLOT(onWifiFailed()));
    timeout.start(timeoutMs);
}

void ServerStats::onWifiFailed() {
    if (!ready || started)
        return;
    timeout.stop();
    started = true;
    nh_log("NickelSync: STATS: Wi-Fi failed");
    showMessage(QStringLiteral("Could not load the server stats: Wi-Fi did not connect."), "error");
}

void ServerStats::load() {
    if (!ready || started)
        return;
    started = true;
    showMessage(QStringLiteral("Loading the server stats"), "loading");
    // Create the manager after Wi-Fi is up. Qt 5.2 can mark a manager that was
    // created offline as not accessible, and it then fails each request.
    QNetworkAccessManager *nam = new QNetworkAccessManager(this);
    nam->setNetworkAccessible(QNetworkAccessManager::Accessible);
    QNetworkRequest req(QUrl(endpoint + QStringLiteral("/v1/library/") +
                             QString::fromLatin1(QUrl::toPercentEncoding(bookId)) + QStringLiteral("/state")));
    req.setHeader(QNetworkRequest::UserAgentHeader, QString::fromLatin1(STATS_AGENT));
    req.setRawHeader("Accept", "application/json");
    reply = nam->get(req);
    connect(reply, SIGNAL(finished()), this, SLOT(onReply()));
    timeout.start(STATS_TIMEOUT_MS);
    nh_log("NickelSync: STATS: GET state");
}

void ServerStats::onTimeout() {
    if (!started) {
        started = true;
        nh_log("NickelSync: STATS: Wi-Fi timed out");
        showMessage(QStringLiteral("Could not load the server stats: Wi-Fi did not connect."), "error");
        return;
    }
    if (!reply)
        return;
    // Disconnect first, so that the abort does not show a second message.
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
    nh_log("NickelSync: STATS: timed out");
    showMessage(QStringLiteral("Could not load the server stats: the server did not answer in %1 s.")
                    .arg(STATS_TIMEOUT_MS / 1000),
                "error");
}

void ServerStats::onReply() {
    QNetworkReply *r = reply.data();
    if (!r)
        return;
    timeout.stop();
    r->deleteLater();
    int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QByteArray body = r->readAll();
    // Log the status only. The URL and the error string have the token in them.
    nh_log("NickelSync: STATS: HTTP %d, error %d, %d bytes", status, int(r->error()), body.size());
    if (status == 200)
        return showStats(body);
    QString why;
    if (status == 404 || (status >= 300 && status < 400))
        why = QStringLiteral("the server does not have this book.");
    else if (status == 401 || status == 403)
        why = QStringLiteral("the server did not accept the sync token.");
    else if (status)
        why = QStringLiteral("the server answered HTTP %1.").arg(status);
    else if (r->error() == QNetworkReply::SslHandshakeFailedError)
        why = QStringLiteral("the TLS connection failed.");
    else
        why = QStringLiteral("no connection to the server.");
    showMessage(QStringLiteral("Could not load the server stats: ") + why, "error");
}

// koboTime reads a Kobo timestamp such as 2026-11-16T21:14:05Z, in UTC.
static QString koboTime(QJsonValue const &v) {
    QString s = v.toString();
    QDateTime t = QDateTime::fromString(s.left(19), QStringLiteral("yyyy-MM-ddTHH:mm:ss"));
    if (!t.isValid())
        return s;
    t.setTimeSpec(Qt::UTC);
    return t.toLocalTime().toString(QStringLiteral("d MMMM yyyy, HH:mm"));
}

static QString minutes(int m) {
    return m >= 60 ? QStringLiteral("%1 h %2 min").arg(m / 60).arg(m % 60) : QStringLiteral("%1 min").arg(m);
}

// showStats shows the fields that the user chose: the position, the reading
// time and the raw location. The answer is an array with one ReadingState.
void ServerStats::showStats(QByteArray const &body) {
    QJsonObject state = QJsonDocument::fromJson(body).array().at(0).toObject();
    if (state.isEmpty()) {
        nh_log("NickelSync: STATS: the answer is not a reading state");
        return showMessage(QStringLiteral("Could not load the server stats: the answer is not a reading state."), "error");
    }
    message->hide();
    setProperty("nsStatsState", QStringLiteral("done"));

    QJsonObject mark = state.value(QStringLiteral("CurrentBookmark")).toObject();
    QJsonValue book = mark.value(QStringLiteral("ProgressPercent"));
    QJsonValue chapter = mark.value(QStringLiteral("ContentSourceProgressPercent"));
    QStringList position;
    if (book.isDouble())
        position << QStringLiteral("%1% of the book").arg(qRound(book.toDouble()));
    if (chapter.isDouble())
        position << QStringLiteral("%1% of the chapter").arg(qRound(chapter.toDouble()));
    addRow(QStringLiteral("Position"), position.isEmpty() ? QStringLiteral("None") : position.join(QStringLiteral(", ")));
    if (mark.contains(QStringLiteral("LastModified")))
        addRow(QStringLiteral("Position changed"), koboTime(mark.value(QStringLiteral("LastModified"))));

    QJsonObject stats = state.value(QStringLiteral("Statistics")).toObject();
    QStringList time;
    if (stats.value(QStringLiteral("SpentReadingMinutes")).isDouble())
        time << minutes(stats.value(QStringLiteral("SpentReadingMinutes")).toInt()) + QStringLiteral(" spent");
    if (stats.value(QStringLiteral("RemainingTimeMinutes")).isDouble())
        time << QStringLiteral("about ") + minutes(stats.value(QStringLiteral("RemainingTimeMinutes")).toInt()) +
                    QStringLiteral(" left");
    addRow(QStringLiteral("Reading time"), time.isEmpty() ? QStringLiteral("None") : time.join(QStringLiteral(", ")));

    QJsonObject loc = mark.value(QStringLiteral("Location")).toObject();
    QString where = loc.value(QStringLiteral("Value")).toString();
    if (where.isEmpty())
        where = QStringLiteral("None");
    // For example "KoboSpan in OEBPS/chapter07.xhtml".
    QStringList detail;
    for (char const *field : {"Type", "Source"})
        if (!loc.value(QString::fromLatin1(field)).toString().isEmpty())
            detail << loc.value(QString::fromLatin1(field)).toString();
    if (!detail.isEmpty())
        where += QStringLiteral("\n") + detail.join(QStringLiteral(" in "));
    addRow(QStringLiteral("Location"), where);
}

void ServerStats::addRow(QString const &key, QString const &value) {
    QLabel *k = new QLabel(key);
    k->setObjectName(QStringLiteral("nsKey"));
    QLabel *v = new QLabel(value);
    v->setObjectName(QStringLiteral("nsValue"));
    v->setWordWrap(true);
    rows->addWidget(k);
    rows->addWidget(v);
    // A layout shows a new child of a visible widget only in the next event
    // loop pass, which is a second paint and so a second e-ink update. Show
    // the rows now, in the same paint as the hide of the message.
    k->show();
    v->show();
}
