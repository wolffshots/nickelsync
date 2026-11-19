// A fake Calibre-Web for the server stats of the NickelSync page. It answers
// GET /kobo/<token>/v1/library/<id>/state on localhost. It also checks the
// rules of CCP and Calibre-Web that the request must pass, and refuses the
// request with 418, as the CCP tarpit does, if one fails.
#include "nickelstub.h"

#include <QDebug>
#include <QHostAddress>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>

static const char TOKEN[] = "simtoken";

static void serverLog(QString const &line) { qInfo().noquote() << "server(sim):" << line; }

static QByteArray stateJson() {
    QByteArray id = simOptions.bookId.toUtf8();
    return "[{\"EntitlementId\":\"" + id + "\",\"Created\":\"2026-10-01T08:00:00Z\","
           "\"LastModified\":\"2026-11-16T21:14:05Z\",\"PriorityTimestamp\":\"2026-11-16T21:14:05Z\","
           "\"StatusInfo\":{\"LastModified\":\"2026-11-02T19:40:11Z\",\"Status\":\"Reading\",\"TimesStartedReading\":1},"
           "\"Statistics\":{\"LastModified\":\"2026-11-16T21:14:05Z\",\"SpentReadingMinutes\":312,\"RemainingTimeMinutes\":148},"
           "\"CurrentBookmark\":{\"LastModified\":\"2026-11-16T21:14:05Z\",\"ProgressPercent\":34,"
           "\"ContentSourceProgressPercent\":61.5,"
           "\"Location\":{\"Value\":\"kobo.18.2\",\"Type\":\"KoboSpan\",\"Source\":\"OEBPS/chapter07.xhtml\"}}}]";
}

static void respond(QTcpSocket *s, int status, QByteArray const &body) {
    if (!s)
        return;
    serverLog(QStringLiteral("HTTP %1, %2 bytes").arg(status).arg(body.size()));
    QByteArray reason = status == 200 ? "OK" : "Error";
    s->write("HTTP/1.1 " + QByteArray::number(status) + " " + reason + "\r\nContent-Type: application/json\r\n"
             "Content-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
    s->disconnectFromHost();
}

// check returns why CCP or Calibre-Web would refuse the request, or an empty
// string.
static QString check(QByteArray const &head) {
    QList<QByteArray> lines = head.split('\n');
    QList<QByteArray> request = lines.value(0).trimmed().split(' ');
    QByteArray want = "/kobo/" + QByteArray(TOKEN) + "/v1/library/" + simOptions.bookId.toUtf8() + "/state";
    if (request.value(0) != "GET")
        return QStringLiteral("method is not GET");
    if (request.value(1) != want)
        return QStringLiteral("unexpected path");
    bool agent = false;
    for (QByteArray const &l : lines) {
        QByteArray lower = l.trimmed().toLower();
        if (lower.startsWith("user-agent:") && lower.contains("kobo"))
            agent = true;
        // With this header, Calibre-Web would treat the request as the Kobo's own.
        if (lower.startsWith("x-kobo-deviceid:"))
            return QStringLiteral("X-Kobo-DeviceId header sent");
    }
    if (!agent)
        return QStringLiteral("the User-Agent has no Kobo, CCP sends it to the tarpit");
    return QString();
}

static void handle(QTcpSocket *s, QByteArray const &head) {
    QString refused = check(head);
    if (!refused.isEmpty()) {
        serverLog(QStringLiteral("refused: ") + refused);
        return respond(s, 418, QByteArray());
    }
    serverLog(QStringLiteral("GET .../state"));
    QPointer<QTcpSocket> p(s);
    switch (simOptions.stats) {
    case SimOptions::StatsOk:
        QTimer::singleShot(400, [p] { respond(p, 200, stateJson()); });
        break;
    case SimOptions::StatsSlow:
        QTimer::singleShot(6000, [p] { respond(p, 200, stateJson()); });
        break;
    case SimOptions::StatsError:
        QTimer::singleShot(400, [p] { respond(p, 500, QByteArray("{}")); });
        break;
    case SimOptions::StatsMissing:
        QTimer::singleShot(400, [p] { respond(p, 404, QByteArray()); });
        break;
    case SimOptions::StatsTimeout:
        serverLog(QStringLiteral("no answer"));
        break;
    case SimOptions::StatsStore:
        break;
    }
}

QString fakeserver_start() {
    if (simOptions.stats == SimOptions::StatsStore)
        return QStringLiteral("https://storeapi.kobo.com");
    QTcpServer *server = new QTcpServer();
    if (!server->listen(QHostAddress::LocalHost, 0)) {
        serverLog(QStringLiteral("cannot listen"));
        return QString();
    }
    QObject::connect(server, &QTcpServer::newConnection, [server] {
        while (QTcpSocket *s = server->nextPendingConnection()) {
            QObject::connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
            QObject::connect(s, &QTcpSocket::readyRead, [s] {
                QByteArray buf = s->property("buf").toByteArray() + s->readAll();
                s->setProperty("buf", buf);
                int end = buf.indexOf("\r\n\r\n");
                if (end >= 0 && !s->property("done").toBool()) {
                    s->setProperty("done", true);
                    handle(s, buf.left(end));
                }
            });
        }
    });
    return QStringLiteral("http://127.0.0.1:%1/kobo/%2").arg(server->serverPort()).arg(QLatin1String(TOKEN));
}
