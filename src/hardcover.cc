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
#include <QFile>
#include <QJsonDocument>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
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
#define HC_CACHE  HC_DIR "/hardcover-cache.json"
#define KOBO_DB   "/mnt/onboard/.kobo/KoboReader.sqlite"

// The sim builds with a mock server. See sim/Makefile.
#ifndef HC_API
#define HC_API "https://api.hardcover.app"
#endif
// The public id of the NickelSync OAuth app ("Mobile, desktop, or CLI", with
// the Device Authorization Grant on, no redirect URIs, and the 4 scopes of
// HC_SCOPE). A public client has no secret.
#ifndef HC_CLIENT_ID
#define HC_CLIENT_ID "f3d2c018-0ac2-46d9-95a9-a746f1046d78"
#endif
#define HC_SCOPE "read:me:content read:library read:journal read:catalog"

#ifndef NH_VERSION
#define NH_VERSION "dev"
#endif

static const int REQUEST_TIMEOUT_MS = 30 * 1000;
static const qint64 CACHE_TTL = 24 * 3600; // seconds
static QString const CACHE_TIME = QStringLiteral("yyyy-MM-ddTHH:mm:ss");

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
    // The cache has data of the account, such as the journal.
    QFile::remove(QStringLiteral(HC_CACHE));
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
                 if (!autoLoad())
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

// ---- cache ---------------------------------------------------------------

// The cache keeps the Hardcover data that the pages show, so a page shows it
// at once, also without Wi-Fi, and then updates it. It is a JSON object of
// entries {"saved": UTC time, "data": value}. The keys are "edition/<ISBN>"
// (the book id), "status/<book id>" and "journal/<book id>/<offset>". An entry
// older than CACHE_TTL does not count, and the next write removes it.

static QJsonObject readCache() {
    QFile f(QStringLiteral(HC_CACHE));
    if (!f.open(QIODevice::ReadOnly))
        return QJsonObject();
    return QJsonDocument::fromJson(f.readAll()).object();
}

static QDateTime savedAt(QJsonValue const &entry) {
    QDateTime t = QDateTime::fromString(entry.toObject().value(QStringLiteral("saved")).toString(), CACHE_TIME);
    t.setTimeSpec(Qt::UTC);
    return t;
}

static bool fresh(QDateTime const &saved) {
    return saved.isValid() && saved.secsTo(QDateTime::currentDateTimeUtc()) <= CACHE_TTL;
}

// cacheGet gets the data of a cache entry that is younger than CACHE_TTL.
static bool cacheGet(QString const &key, QJsonValue &data, QDateTime &saved) {
    QJsonValue e = readCache().value(key);
    saved = savedAt(e);
    if (!fresh(saved))
        return false;
    data = e.toObject().value(QStringLiteral("data"));
    return true;
}

// cachePut saves the data of a key, and removes the old entries.
static void cachePut(QString const &key, QJsonValue const &data) {
    QJsonObject cache = readCache();
    for (QJsonObject::iterator it = cache.begin(); it != cache.end();)
        it = fresh(savedAt(it.value())) ? it + 1 : cache.erase(it);
    QJsonObject e;
    e.insert(QStringLiteral("saved"), QDateTime::currentDateTimeUtc().toString(CACHE_TIME));
    e.insert(QStringLiteral("data"), data);
    cache.insert(key, e);
    QSaveFile f(QStringLiteral(HC_CACHE));
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(cache).toJson(QJsonDocument::Compact)) < 0 || !f.commit())
        hcLog("CACHE: cannot write " HC_CACHE);
}

// ---- queries -------------------------------------------------------------

static char const EDITION_QUERY[] =
    "query NickelSyncEdition($isbn: [String!]!) {"
    " editions(where: {_or: [{isbn_13: {_in: $isbn}}, {isbn_10: {_in: $isbn}}, {asin: {_in: $isbn}}]}, limit: 1)"
    " { book_id } }";

static char const STATUS_QUERY[] =
    "query NickelSyncStatus($book: Int!) {"
    " me { id user_books(where: {book_id: {_eq: $book}}, limit: 1) {"
    " updated_at rating read_count user_book_status { status }"
    " user_book_reads(order_by: {id: desc}, limit: 1)"
    " { progress progress_pages started_at finished_at edition { pages } } } }"
    " books_by_pk(id: $book) { title subtitle description pages release_year rating ratings_count"
    " users_read_count cached_tags featured_book_series { position series { name } }"
    " contributions(limit: 6) { contribution author { name } } } }";

static char const JOURNAL_QUERY[] =
    "query NickelSyncJournal($book: Int!, $user: Int!, $limit: Int!, $offset: Int!) {"
    " reading_journals(where: {user_id: {_eq: $user}, book_id: {_eq: $book}},"
    " order_by: {action_at: desc}, limit: $limit, offset: $offset)"
    " { id event entry action_at metadata } }";

static char const SEARCH_QUERY[] =
    "query NickelSyncSearch($query: String!, $limit: Int!, $page: Int!) {"
    " search(query: $query, query_type: \"Book\", per_page: $limit, page: $page) { results } }";

// load finds the open book on Hardcover and gets its status. A cached status
// shows at once, and the reply of Hardcover updates it.
void Hardcover::load() {
    message.clear();
    statusCache = CacheState();
    QJsonValue cached;
    QDateTime saved;
    int id = linkedBook();
    QString isbn;
    if (id <= 0) {
        isbn = isbnOf(book.contentId);
        if (!isbn.isEmpty() && cacheGet(QStringLiteral("edition/") + isbn, cached, saved))
            id = cached.toInt();
    }
    if (id > 0 && cacheGet(QStringLiteral("status/%1").arg(id), cached, saved)) {
        hcLog(QStringLiteral("LOAD: book %1 from the cache").arg(id));
        applyStatus(id, cached.toObject());
        loadState = Loaded;
        statusCache.saved = saved;
        statusCache.updating = true;
    }
    if (!wifiConnected()) {
        if (statusCache.saved.isValid()) {
            statusCache.updating = false;
            statusCache.error = QStringLiteral("Wi-Fi is off.");
        } else {
            loadState = NotLoaded;
            message = QStringLiteral("Connect to Wi-Fi first.");
        }
        rebuild();
        return;
    }
    if (!statusCache.saved.isValid())
        loadState = Loading;
    rebuild();
    if (id > 0) {
        hcLog(QStringLiteral("LOAD: book %1").arg(id));
        loadStatus(id);
        return;
    }
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
    graphql(EDITION_QUERY, vars, [this, isbn](QJsonObject const &data, QString const &error) {
        if (!error.isEmpty())
            return loadFailed(error);
        QJsonArray editions = data.value(QStringLiteral("editions")).toArray();
        int found = editions.isEmpty() ? 0 : editions.first().toObject().value(QStringLiteral("book_id")).toInt();
        if (found <= 0) {
            loadState = NotFound;
            rebuild();
            return;
        }
        cachePut(QStringLiteral("edition/") + isbn, found);
        loadStatus(found);
    });
}

// loadFailed shows the error of a load. A cached status stays, and its cache
// line shows the error.
void Hardcover::loadFailed(QString const &error) {
    if (statusCache.saved.isValid()) {
        statusCache.updating = false;
        statusCache.error = error;
    } else {
        loadState = Failed;
        message = error;
    }
    rebuild();
}

static QDate dateOf(QJsonValue const &v) { return QDate::fromString(v.toString().left(10), Qt::ISODate); }

// numberOf reads a Hasura number. A numeric column can come as a string.
static double numberOf(QJsonValue const &v) { return v.toVariant().toDouble(); }

// bookAuthors gets the authors of a book. A contribution without a role
// (translator, illustrator) is an author.
static QString bookAuthors(QJsonObject const &b) {
    QStringList names;
    for (QJsonValue const &c : b.value(QStringLiteral("contributions")).toArray()) {
        QJsonObject o = c.toObject();
        QString name = o.value(QStringLiteral("author")).toObject().value(QStringLiteral("name")).toString();
        if (o.value(QStringLiteral("contribution")).isNull() && !name.isEmpty() && names.size() < 3)
            names << name;
    }
    return names.join(QStringLiteral(", "));
}

// bookSeries gets the featured series and the position, "Name #2".
static QString bookSeries(QJsonObject const &b) {
    QJsonObject fs = b.value(QStringLiteral("featured_book_series")).toObject();
    QString series = fs.value(QStringLiteral("series")).toObject().value(QStringLiteral("name")).toString();
    if (!series.isEmpty() && fs.value(QStringLiteral("position")).isDouble())
        series += QStringLiteral(" #") + QString::number(fs.value(QStringLiteral("position")).toDouble());
    return series;
}

// bookGenres gets the first genres. cached_tags is
// {"Genre": [{"tag": "Fantasy", ...}], "Mood": [...], ...}.
static QString bookGenres(QJsonObject const &b) {
    QStringList tags;
    for (QJsonValue const &t : b.value(QStringLiteral("cached_tags")).toObject().value(QStringLiteral("Genre")).toArray()) {
        QString tag = t.toObject().value(QStringLiteral("tag")).toString();
        if (!tag.isEmpty() && tags.size() < 4)
            tags << tag;
    }
    return tags.join(QStringLiteral(", "));
}

// bookDescription gets the description. An e-ink page does not scroll, so it
// is short.
static QString bookDescription(QJsonObject const &b) {
    QString d = b.value(QStringLiteral("description")).toString().simplified();
    if (d.size() > 360)
        d = d.left(360).trimmed() + QStringLiteral("...");
    return d;
}

void Hardcover::loadStatus(int id) {
    QJsonObject vars;
    vars.insert(QStringLiteral("book"), id);
    graphql(STATUS_QUERY, vars, [this, id](QJsonObject const &data, QString const &error) {
        if (!error.isEmpty())
            return loadFailed(error);
        cachePut(QStringLiteral("status/%1").arg(id), data);
        applyStatus(id, data);
        hcLog(QStringLiteral("LOADED: book %1, in library %2").arg(id).arg(status.inLibrary));
        statusCache = CacheState();
        loadState = Loaded;
        rebuild();
    });
}

// applyStatus reads the reply of the status query, from Hardcover or from the
// cache.
void Hardcover::applyStatus(int id, QJsonObject const &data) {
    bookId = id;
    status = Status();
    QJsonObject b = data.value(QStringLiteral("books_by_pk")).toObject();
    status.bookTitle = b.value(QStringLiteral("title")).toString();
    status.subtitle = b.value(QStringLiteral("subtitle")).toString();
    status.authors = bookAuthors(b);
    status.series = bookSeries(b);
    status.genres = bookGenres(b);
    status.description = bookDescription(b);
    status.year = b.value(QStringLiteral("release_year")).toInt();
    status.bookPages = b.value(QStringLiteral("pages")).toInt();
    status.rating = numberOf(b.value(QStringLiteral("rating")));
    status.ratings = b.value(QStringLiteral("ratings_count")).toInt();
    status.readers = b.value(QStringLiteral("users_read_count")).toInt();
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
        status.userRating = numberOf(ub.value(QStringLiteral("rating")));
        status.readCount = ub.value(QStringLiteral("read_count")).toInt();
        QJsonArray reads = ub.value(QStringLiteral("user_book_reads")).toArray();
        if (!reads.isEmpty()) {
            QJsonObject r = reads.first().toObject();
            QJsonValue p = r.value(QStringLiteral("progress"));
            status.progress = p.isDouble() ? p.toDouble() : -1;
            status.pages = r.value(QStringLiteral("progress_pages")).toInt();
            status.started = dateOf(r.value(QStringLiteral("started_at")));
            status.finished = dateOf(r.value(QStringLiteral("finished_at")));
            status.editionPages = r.value(QStringLiteral("edition")).toObject().value(QStringLiteral("pages")).toInt();
        }
    }
}

// loadJournal gets the journal entries of the user for the book, from
// journalOffset on. It gets more entries than a page can show, so
// rebuildJournal knows if an older page exists. openJournal shows them.
// A cached page shows at once. If the reply has the same entries, only the
// cache line changes, else the page is built again.
void Hardcover::loadJournal() {
    QString key = QStringLiteral("journal/%1/%2").arg(bookId).arg(journalOffset);
    int gen = ++journalGen;
    journalError.clear();
    journalCache = CacheState();
    QJsonValue cached;
    if (cacheGet(key, cached, journalCache.saved)) {
        journal = cached.toArray();
        journalCache.updating = true;
    } else {
        journalCache.saved = QDateTime();
    }
    bool fromCache = journalCache.saved.isValid();
    if (!wifiConnected()) {
        if (fromCache) {
            journalCache.updating = false;
            journalCache.error = QStringLiteral("Wi-Fi is off.");
        } else {
            journalError = QStringLiteral("Connect to Wi-Fi first.");
        }
        openJournal();
        return;
    }
    if (fromCache)
        openJournal();
    QSettings store(QStringLiteral(HC_STORE), QSettings::IniFormat);
    QJsonObject vars;
    vars.insert(QStringLiteral("book"), bookId);
    vars.insert(QStringLiteral("user"), store.value(QStringLiteral("auth/user_id")).toInt());
    vars.insert(QStringLiteral("limit"), JOURNAL_FETCH + 1);
    vars.insert(QStringLiteral("offset"), journalOffset);
    journalLoading = true;
    // Without a cached page, show no "Loading" page. The page opens when the
    // entries arrive, and the entries of a page stay until the next page
    // arrives. A "Loading" page between them is one more e-ink update, of
    // most of the screen.
    graphql(JOURNAL_QUERY, vars, [this, key, gen, fromCache](QJsonObject const &data, QString const &error) {
        QJsonArray entries = data.value(QStringLiteral("reading_journals")).toArray();
        if (error.isEmpty())
            cachePut(key, entries);
        if (gen != journalGen) // a newer request
            return;
        journalLoading = false;
        if (fromCache && !journalView) // the user closed the cached page
            return;
        hcLog(QStringLiteral("JOURNAL: offset %1, %2 entries, error \"%3\"").arg(journalOffset).arg(entries.size()).arg(error));
        if (fromCache && !error.isEmpty()) {
            journalCache.updating = false;
            journalCache.error = error;
            updateJournalNote();
            return;
        }
        bool same = fromCache && entries == journal;
        journalCache = CacheState();
        journalError = error;
        if (same) {
            updateJournalNote();
            return;
        }
        // rebuildJournal shows the entries that fit and sets journalMore.
        journal = entries;
        openJournal();
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
    // Without the link, the ISBN can still find the book.
    loadState = NotLoaded;
    load();
}

// autoLoad loads the open book each time the page opens, so the user does not
// have to tap "Load from Hardcover". A cached status shows at once, and the
// load updates it. It returns true if it started a load, which rebuilds the
// page.
bool Hardcover::autoLoad() {
    if (auth != SignedIn || book.contentId.isEmpty() || loadState == Loading)
        return false;
    load();
    return true;
}
