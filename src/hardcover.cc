// Hardcover: the sign-in, the token store and the queries. The pages are in
// hardcoverpage.cc.
//
// Sign-in: the OAuth 2 Device Authorization Grant of Hardcover
// (docs.hardcover.app/api/oauth). NickelSync asks for a code, the page shows
// it, and the user enters it at hardcover.app/link on a phone. NickelSync
// polls the token endpoint until the user approves. The scopes are read-only.
//
// Tokens: an access token lasts 1 week, a refresh token 6 months. Each refresh
// gives a new refresh token, and the old one stops working. If a refresh token
// is used twice, Hardcover revokes the session. So save the new tokens at once.
// The tokens are in hardcover.ini next to config.ini. Never log them.
//
// Book match, as NickelHardcover does it: a link that the user picked on the
// "Link book" page, else the ISBN of the book in KoboReader.sqlite.

#include <QCoreApplication>
#include <QDir>
#include <QJsonDocument>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUrl>
#include <QVariant>
#include <QWidget>

#include <NickelHook.h>

#include "hardcover.h"
#include "nickel.h"
#include "settings.h"

#define HC_DIR    "/mnt/onboard/.adds/nickelsync"
#define HC_STORE  HC_DIR "/hardcover.ini"
#define KOBO_DB   "/mnt/onboard/.kobo/KoboReader.sqlite"

// The sim builds with a mock server. See sim/Makefile.
#ifndef HC_API
#define HC_API "https://api.hardcover.app"
#endif
// The public id of the NickelSync OAuth app ("Mobile, desktop, or CLI", with
// the Device Authorization Grant on). A public client has no secret.
// ponytail: a placeholder until the app is registered. Sign-in fails with
// invalid_client until then. Put the real id here.
#ifndef HC_CLIENT_ID
#define HC_CLIENT_ID "nickelsync-client-id-placeholder"
#endif
#define HC_SCOPE "read:me:content read:library read:journal read:catalog"

#ifndef NH_VERSION
#define NH_VERSION "dev"
#endif

static const int REQUEST_TIMEOUT_MS = 30 * 1000;

static void hcLog(QString const &line) { nh_log("NickelSync: HC: %s", qPrintable(line)); }

QString (*Content__getId)(void const *);
QString (*Content__getTitle)(void const *);
QString (*Content__getAttribution)(void const *);

Hardcover *Hardcover::instance() {
    static Hardcover *hc = new Hardcover(QCoreApplication::instance());
    return hc;
}

bool Hardcover::enabled() {
    return settingBool("Hardcover");
}

Hardcover::Hardcover(QObject *parent) : QObject(parent), net(new QNetworkAccessManager(this)) {
    poll.setSingleShot(true);
    connect(&poll, SIGNAL(timeout()), this, SLOT(onPoll()));
    QSettings store(QStringLiteral(HC_STORE), QSettings::IniFormat);
    auth = store.value(QStringLiteral("auth/refresh_token")).toString().isEmpty() ? SignedOut : SignedIn;
}

bool Hardcover::wifiConnected() {
    return WirelessManager__connected(WirelessManager__sharedInstance());
}

// ---- requests ----------------------------------------------------------

// post sends a POST request. done gets the HTTP status and the JSON body. A
// status of 0 is a network error, and the body then has "error".
void Hardcover::post(QString const &url, QByteArray const &body, bool json, bool withToken, Reply done) {
    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::ContentTypeHeader,
                  json ? "application/json" : "application/x-www-form-urlencoded");
    req.setRawHeader("User-Agent", "NickelSync/" NH_VERSION " (Kobo)");
    if (withToken) {
        QSettings store(QStringLiteral(HC_STORE), QSettings::IniFormat);
        req.setRawHeader("Authorization", "Bearer " + store.value(QStringLiteral("auth/access_token")).toByteArray());
    }
    QNetworkReply *reply = net->post(req, body);
    QTimer *timer = new QTimer(reply);
    timer->setSingleShot(true);
    connect(timer, SIGNAL(timeout()), reply, SLOT(abort()));
    timer->start(REQUEST_TIMEOUT_MS);
    connect(reply, &QNetworkReply::finished, [reply, done] {
        reply->deleteLater();
        int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QJsonObject obj = QJsonDocument::fromJson(reply->readAll()).object();
        if (status == 0) {
            obj = QJsonObject();
            obj.insert(QStringLiteral("error"), reply->error() == QNetworkReply::OperationCanceledError
                                                    ? QStringLiteral("Hardcover did not answer")
                                                    : QStringLiteral("No connection to Hardcover"));
        }
        done(status, obj);
    });
}

// form encodes a body as application/x-www-form-urlencoded.
static QByteArray form(QList<QPair<QString, QString>> const &items) {
    QByteArray out;
    for (auto const &i : items) {
        if (!out.isEmpty())
            out += '&';
        out += QUrl::toPercentEncoding(i.first) + '=' + QUrl::toPercentEncoding(i.second);
    }
    return out;
}

static QString errorText(int status, QJsonObject const &body) {
    QString e = body.value(QStringLiteral("error_description")).toString();
    if (e.isEmpty())
        e = body.value(QStringLiteral("error")).toString();
    if (status == 429)
        return QStringLiteral("Hardcover rate limit. Try again later.");
    return e.isEmpty() ? QStringLiteral("Hardcover error %1").arg(status) : e;
}

// graphql sends a query with the access token. It refreshes the token first if
// it is about to expire, and once more after a 401.
void Hardcover::graphql(char const *query, QJsonObject const &vars, Result done, bool retried) {
    QSettings store(QStringLiteral(HC_STORE), QSettings::IniFormat);
    QDateTime expires = store.value(QStringLiteral("auth/expires_at")).toDateTime();
    if (!retried && (!expires.isValid() || expires < QDateTime::currentDateTimeUtc().addSecs(60))) {
        refresh([this, query, vars, done](bool ok) {
            if (ok)
                graphql(query, vars, done, true);
            else
                done(QJsonObject(), refreshError());
        });
        return;
    }
    QJsonObject body;
    body.insert(QStringLiteral("query"), QString::fromLatin1(query));
    body.insert(QStringLiteral("variables"), vars);
    post(QStringLiteral(HC_API "/v1/graphql"), QJsonDocument(body).toJson(QJsonDocument::Compact), true, true,
         [this, query, vars, done, retried](int status, QJsonObject const &reply) {
             if (status == 401 && !retried) {
                 refresh([this, query, vars, done](bool ok) {
                     if (ok)
                         graphql(query, vars, done, true);
                     else
                         done(QJsonObject(), refreshError());
                 });
                 return;
             }
             if (status != 200)
                 return done(QJsonObject(), errorText(status, reply));
             QJsonArray errors = reply.value(QStringLiteral("errors")).toArray();
             if (!errors.isEmpty()) {
                 QJsonValue e = errors.first();
                 QString text = e.isObject() ? e.toObject().value(QStringLiteral("message")).toString() : e.toString();
                 hcLog("QUERY_ERROR: " + text);
                 return done(QJsonObject(), text.isEmpty() ? QStringLiteral("Hardcover query failed") : text);
             }
             done(reply.value(QStringLiteral("data")).toObject(), QString());
         });
}

// refreshError is the error text after a failed refresh. A network error
// keeps the tokens, so the user stays signed in.
QString Hardcover::refreshError() const {
    return auth == SignedIn ? QStringLiteral("No connection to Hardcover") : QStringLiteral("Sign in to Hardcover again.");
}

// refresh gets new tokens with the refresh token. Calls that come while a
// refresh runs wait for it, so a refresh token is never used twice.
void Hardcover::refresh(std::function<void(bool ok)> done) {
    refreshWaiters << done;
    if (refreshing)
        return;
    QSettings store(QStringLiteral(HC_STORE), QSettings::IniFormat);
    QString token = store.value(QStringLiteral("auth/refresh_token")).toString();
    auto finish = [this](bool ok) {
        refreshing = false;
        QList<std::function<void(bool)>> waiters = refreshWaiters;
        refreshWaiters.clear();
        for (auto const &w : waiters)
            w(ok);
    };
    if (token.isEmpty()) {
        clearTokens();
        finish(false);
        return;
    }
    refreshing = true;
    hcLog("REFRESH");
    post(QStringLiteral(HC_API "/oauth2/token"),
         form({{"grant_type", "refresh_token"}, {"refresh_token", token}, {"client_id", HC_CLIENT_ID}}), false, false,
         [this, finish](int status, QJsonObject const &body) {
             if (status == 200 && !body.value(QStringLiteral("access_token")).toString().isEmpty()) {
                 saveTokens(body);
                 finish(true);
                 return;
             }
             hcLog(QStringLiteral("REFRESH_FAILED: %1 %2").arg(status).arg(body.value(QStringLiteral("error")).toString()));
             // A network error keeps the tokens. Any answer of the server
             // means that the refresh token is no longer valid.
             if (status != 0) {
                 clearTokens();
                 message = QStringLiteral("Sign in to Hardcover again.");
             }
             finish(false);
         });
}

void Hardcover::saveTokens(QJsonObject const &body) {
    QSettings store(QStringLiteral(HC_STORE), QSettings::IniFormat);
    int expiresIn = body.value(QStringLiteral("expires_in")).toInt(7 * 24 * 3600);
    store.setValue(QStringLiteral("auth/access_token"), body.value(QStringLiteral("access_token")).toString());
    QString rt = body.value(QStringLiteral("refresh_token")).toString();
    if (!rt.isEmpty())
        store.setValue(QStringLiteral("auth/refresh_token"), rt);
    store.setValue(QStringLiteral("auth/expires_at"), QDateTime::currentDateTimeUtc().addSecs(expiresIn));
    store.sync();
    auth = SignedIn;
}

// clearTokens removes the tokens. The links stay.
void Hardcover::clearTokens() {
    QSettings store(QStringLiteral(HC_STORE), QSettings::IniFormat);
    store.remove(QStringLiteral("auth"));
    store.sync();
    auth = SignedOut;
    loadState = NotLoaded;
    rebuild();
}

// ---- sign-in -------------------------------------------------------------

void Hardcover::startSignIn() {
    if (!wifiConnected()) {
        message = QStringLiteral("Connect to Wi-Fi first.");
        rebuild();
        return;
    }
    message.clear();
    auth = SigningIn;
    userCode.clear();
    rebuild();
    hcLog("SIGNIN: device code");
    post(QStringLiteral(HC_API "/oauth2/device"), form({{"client_id", HC_CLIENT_ID}, {"scope", HC_SCOPE}}), false, false,
         [this](int status, QJsonObject const &body) {
             if (auth != SigningIn)
                 return;
             deviceCode = body.value(QStringLiteral("device_code")).toString();
             userCode = body.value(QStringLiteral("user_code")).toString();
             verifyUrl = body.value(QStringLiteral("verification_uri")).toString();
             if (status != 200 || deviceCode.isEmpty() || userCode.isEmpty()) {
                 hcLog(QStringLiteral("SIGNIN_FAILED: %1").arg(status));
                 auth = SignedOut;
                 message = QStringLiteral("Sign-in failed: ") + errorText(status, body);
                 rebuild();
                 return;
             }
             if (verifyUrl.isEmpty())
                 verifyUrl = QStringLiteral("https://hardcover.app/link");
             pollSeconds = qMax(1, body.value(QStringLiteral("interval")).toInt(5));
             pollDeadline = QDateTime::currentDateTimeUtc().addSecs(body.value(QStringLiteral("expires_in")).toInt(600));
             poll.start(pollSeconds * 1000);
             rebuild();
         });
}

void Hardcover::onPoll() {
    if (auth != SigningIn)
        return;
    if (QDateTime::currentDateTimeUtc() > pollDeadline) {
        auth = SignedOut;
        message = QStringLiteral("The code expired. Sign in again.");
        rebuild();
        return;
    }
    post(QStringLiteral(HC_API "/oauth2/token"),
         form({{"grant_type", "urn:ietf:params:oauth:grant-type:device_code"},
               {"device_code", deviceCode},
               {"client_id", HC_CLIENT_ID}}),
         false, false, [this](int status, QJsonObject const &body) {
             if (auth != SigningIn)
                 return;
             QString error = body.value(QStringLiteral("error")).toString();
             if (status == 200 && !body.value(QStringLiteral("access_token")).toString().isEmpty()) {
                 hcLog("SIGNIN: done");
                 saveTokens(body);
                 message.clear();
                 rebuild();
                 return;
             }
             // A network error is not the end of the flow. Poll again.
             if (status == 0 || error == QLatin1String("authorization_pending") || error == QLatin1String("slow_down")) {
                 if (error == QLatin1String("slow_down"))
                     pollSeconds += 5;
                 poll.start(pollSeconds * 1000);
                 return;
             }
             hcLog("SIGNIN_FAILED: " + error);
             auth = SignedOut;
             message = error == QLatin1String("access_denied") ? QStringLiteral("Sign-in was denied.")
                                                                : QStringLiteral("Sign-in failed: ") + errorText(status, body);
             rebuild();
         });
}

void Hardcover::cancelSignIn() {
    poll.stop();
    auth = SignedOut;
    message.clear();
    rebuild();
}

// signOut revokes the tokens at Hardcover, so the session leaves the list of
// authorized apps, and removes them here.
void Hardcover::signOut() {
    QSettings store(QStringLiteral(HC_STORE), QSettings::IniFormat);
    QString token = store.value(QStringLiteral("auth/refresh_token")).toString();
    if (!token.isEmpty() && wifiConnected())
        post(QStringLiteral(HC_API "/oauth2/revoke"),
             form({{"token", token}, {"token_type_hint", "refresh_token"}, {"client_id", HC_CLIENT_ID}}), false, false,
             [](int status, QJsonObject const &) { hcLog(QStringLiteral("REVOKE: %1").arg(status)); });
    hcLog("SIGNOUT");
    message.clear();
    clearTokens();
}

// ---- the open book -------------------------------------------------------

Hardcover::Book Hardcover::openBook() {
    Book b;
    if (!MainWindowController__viewWithObjectName || !ReadingView__getVolume || !Content__getId)
        return b;
    QWidget *rv = MainWindowController__viewWithObjectName(MainWindowController__sharedInstance(),
                                                           QStringLiteral("ReadingView"));
    // moc generates qt_metacast, so the pointer is right for any base layout.
    void *reading = rv ? rv->qt_metacast("ReadingView") : nullptr;
    if (!reading)
        return b;
    void const *volume = ReadingView__getVolume(reading);
    b.contentId = Content__getId(volume);
    if (Content__getTitle)
        b.title = Content__getTitle(volume);
    if (Content__getAttribution)
        b.author = Content__getAttribution(volume);
    return b;
}

// linkKey is the key of the open book in the [links] group. QSettings uses
// "/" as the group separator, so replace it, as NickelHardcover does.
QString Hardcover::linkKey() const {
    QString id = book.contentId;
    return QStringLiteral("links/") + id.replace(QLatin1Char('/'), QLatin1Char('-')).replace(QLatin1Char('\\'), QLatin1Char('-'));
}

int Hardcover::linkedBook() const {
    QSettings store(QStringLiteral(HC_STORE), QSettings::IniFormat);
    return store.value(linkKey()).toInt();
}

// isbnOf reads the ISBN of a book from the Nickel database, read-only.
static QString isbnOf(QString const &contentId) {
    QString isbn;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), QStringLiteral("nickelsync-hc"));
        db.setDatabaseName(QStringLiteral(KOBO_DB));
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (db.open()) {
            QSqlQuery q(db);
            q.prepare(QStringLiteral("SELECT ISBN FROM content WHERE ContentID = ? AND ContentType = 6"));
            q.addBindValue(contentId);
            if (q.exec() && q.next())
                isbn = q.value(0).toString();
            db.close();
        } else {
            hcLog("DB: cannot open " KOBO_DB);
        }
    }
    QSqlDatabase::removeDatabase(QStringLiteral("nickelsync-hc"));
    isbn.remove(QLatin1Char('-')).remove(QLatin1Char(' '));
    return isbn.trimmed();
}

// ---- queries -------------------------------------------------------------

static char const EDITION_QUERY[] =
    "query NickelSyncEdition($isbn: [String!]!) {"
    " editions(where: {_or: [{isbn_13: {_in: $isbn}}, {isbn_10: {_in: $isbn}}, {asin: {_in: $isbn}}]}, limit: 1)"
    " { book_id } }";

static char const STATUS_QUERY[] =
    "query NickelSyncStatus($book: Int!) {"
    " me { id user_books(where: {book_id: {_eq: $book}}, limit: 1) {"
    " updated_at user_book_status { status }"
    " user_book_reads(order_by: {id: desc}, limit: 1) { progress progress_pages started_at finished_at } } }"
    " books_by_pk(id: $book) { title } }";

static char const JOURNAL_QUERY[] =
    "query NickelSyncJournal($book: Int!, $user: Int!, $limit: Int!, $offset: Int!) {"
    " reading_journals(where: {user_id: {_eq: $user}, book_id: {_eq: $book}},"
    " order_by: {action_at: desc}, limit: $limit, offset: $offset)"
    " { id event entry action_at metadata } }";

static char const SEARCH_QUERY[] =
    "query NickelSyncSearch($query: String!, $limit: Int!, $page: Int!) {"
    " search(query: $query, query_type: \"Book\", per_page: $limit, page: $page) { results } }";

// load finds the open book on Hardcover and gets its status.
void Hardcover::load() {
    if (!wifiConnected()) {
        message = QStringLiteral("Connect to Wi-Fi first.");
        rebuild();
        return;
    }
    message.clear();
    loadState = Loading;
    rebuild();
    int linked = linkedBook();
    if (linked > 0) {
        hcLog(QStringLiteral("LOAD: linked book %1").arg(linked));
        loadStatus(linked);
        return;
    }
    QString isbn = isbnOf(book.contentId);
    if (isbn.isEmpty()) {
        hcLog("LOAD: no ISBN, no link");
        loadState = NotFound;
        rebuild();
        return;
    }
    hcLog("LOAD: ISBN " + isbn);
    QJsonObject vars;
    QJsonArray isbns;
    isbns.append(isbn);
    vars.insert(QStringLiteral("isbn"), isbns);
    graphql(EDITION_QUERY, vars, [this](QJsonObject const &data, QString const &error) {
        if (!error.isEmpty()) {
            loadState = Failed;
            message = error;
            rebuild();
            return;
        }
        QJsonArray editions = data.value(QStringLiteral("editions")).toArray();
        int id = editions.isEmpty() ? 0 : editions.first().toObject().value(QStringLiteral("book_id")).toInt();
        if (id <= 0) {
            loadState = NotFound;
            rebuild();
            return;
        }
        loadStatus(id);
    });
}

static QDate dateOf(QJsonValue const &v) { return QDate::fromString(v.toString().left(10), Qt::ISODate); }

void Hardcover::loadStatus(int id) {
    QJsonObject vars;
    vars.insert(QStringLiteral("book"), id);
    graphql(STATUS_QUERY, vars, [this, id](QJsonObject const &data, QString const &error) {
        if (!error.isEmpty()) {
            loadState = Failed;
            message = error;
            rebuild();
            return;
        }
        bookId = id;
        status = Status();
        status.bookTitle = data.value(QStringLiteral("books_by_pk")).toObject().value(QStringLiteral("title")).toString();
        QJsonArray users = data.value(QStringLiteral("me")).toArray();
        QJsonObject me = users.isEmpty() ? QJsonObject() : users.first().toObject();
        QSettings(QStringLiteral(HC_STORE), QSettings::IniFormat)
            .setValue(QStringLiteral("auth/user_id"), me.value(QStringLiteral("id")).toInt());
        QJsonArray ubs = me.value(QStringLiteral("user_books")).toArray();
        if (!ubs.isEmpty()) {
            QJsonObject ub = ubs.first().toObject();
            status.inLibrary = true;
            status.status = ub.value(QStringLiteral("user_book_status")).toObject().value(QStringLiteral("status")).toString();
            status.updated = dateOf(ub.value(QStringLiteral("updated_at")));
            QJsonArray reads = ub.value(QStringLiteral("user_book_reads")).toArray();
            if (!reads.isEmpty()) {
                QJsonObject r = reads.first().toObject();
                QJsonValue p = r.value(QStringLiteral("progress"));
                status.progress = p.isDouble() ? p.toDouble() : -1;
                status.pages = r.value(QStringLiteral("progress_pages")).toInt();
                status.started = dateOf(r.value(QStringLiteral("started_at")));
                status.finished = dateOf(r.value(QStringLiteral("finished_at")));
            }
        }
        hcLog(QStringLiteral("LOADED: book %1, in library %2").arg(id).arg(status.inLibrary));
        loadState = Loaded;
        rebuild();
    });
}

// loadJournal gets one page of the journal entries of the user for the book.
// It asks for one entry more than a page, to know if an older page exists.
void Hardcover::loadJournal() {
    if (!wifiConnected()) {
        journalError = QStringLiteral("Connect to Wi-Fi first.");
        rebuildJournal();
        return;
    }
    QSettings store(QStringLiteral(HC_STORE), QSettings::IniFormat);
    QJsonObject vars;
    vars.insert(QStringLiteral("book"), bookId);
    vars.insert(QStringLiteral("user"), store.value(QStringLiteral("auth/user_id")).toInt());
    vars.insert(QStringLiteral("limit"), JOURNAL_PAGE + 1);
    vars.insert(QStringLiteral("offset"), journalOffset);
    journalLoading = true;
    journalError.clear();
    // When entries show, keep them until the next page arrives. A "Loading"
    // page between them is one more e-ink update.
    if (journal.isEmpty())
        rebuildJournal();
    graphql(JOURNAL_QUERY, vars, [this](QJsonObject const &data, QString const &error) {
        journalLoading = false;
        journalError = error;
        journal = data.value(QStringLiteral("reading_journals")).toArray();
        journalMore = journal.size() > JOURNAL_PAGE;
        while (journal.size() > JOURNAL_PAGE)
            journal.removeLast();
        hcLog(QStringLiteral("JOURNAL: offset %1, %2 entries").arg(journalOffset).arg(journal.size()));
        rebuildJournal();
    });
}

// search searches the Hardcover catalog for the title and author of the book.
// ponytail: the query is fixed. A query that the user types needs the Nickel
// keyboard. If the right book is not in the results, the user links it with
// NickelHardcover or adds the ISBN in calibre.
void Hardcover::search() {
    if (!wifiConnected()) {
        searchError = QStringLiteral("Connect to Wi-Fi first.");
        rebuildSearch();
        return;
    }
    QJsonObject vars;
    vars.insert(QStringLiteral("query"), (book.title + QLatin1Char(' ') + book.author).trimmed());
    vars.insert(QStringLiteral("limit"), SEARCH_PAGE);
    vars.insert(QStringLiteral("page"), searchPageNo);
    searchLoading = true;
    searchError.clear();
    // When results show, keep them until the next page arrives, as for the
    // journal.
    if (results.isEmpty())
        rebuildSearch();
    graphql(SEARCH_QUERY, vars, [this](QJsonObject const &data, QString const &error) {
        searchLoading = false;
        searchError = error;
        QJsonObject res = data.value(QStringLiteral("search")).toObject().value(QStringLiteral("results")).toObject();
        results = QJsonArray();
        for (QJsonValue const &hit : res.value(QStringLiteral("hits")).toArray())
            results.append(hit.toObject().value(QStringLiteral("document")));
        searchMore = res.value(QStringLiteral("found")).toInt() > searchPageNo * SEARCH_PAGE;
        hcLog(QStringLiteral("SEARCH: page %1, %2 results").arg(searchPageNo).arg(results.size()));
        rebuildSearch();
    });
}

void Hardcover::link(int id) {
    QSettings store(QStringLiteral(HC_STORE), QSettings::IniFormat);
    store.setValue(linkKey(), id);
    store.sync();
    hcLog(QStringLiteral("LINK: book %1").arg(id));
    // Load when the search page is gone. A load before that paints the
    // Hardcover page below the search page: one more e-ink update.
    if (searchView) {
        connect(searchView, SIGNAL(destroyed()), this, SLOT(load()), Qt::QueuedConnection);
        searchView->deleteLater();
    } else {
        load();
    }
}

void Hardcover::unlink() {
    QSettings store(QStringLiteral(HC_STORE), QSettings::IniFormat);
    store.remove(linkKey());
    store.sync();
    hcLog("UNLINK");
    loadState = NotLoaded;
    rebuild();
}
