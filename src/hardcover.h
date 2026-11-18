#ifndef NICKELSYNC_HARDCOVER_H
#define NICKELSYNC_HARDCOVER_H

#include <QDate>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSize>
#include <QString>
#include <QTimer>

#include <functional>

class QBoxLayout;
class QLabel;
class QLayout;
class QNetworkAccessManager;
class QVBoxLayout;
class QWidget;

// Hardcover shows Hardcover.app data of the open book on its own page: the
// library status and the reading progress, and a reading journal page. A
// "Hardcover" button on the NickelSync page opens the page.
// It is read-only. NickelSync signs in with its own OAuth app (device code
// flow), so it does not use the token of NickelHardcover.
//
// hardcover.cc has the sign-in, the token store and the queries.
// hardcoverpage.cc has the button and the pages.
class Hardcover : public QObject {
    Q_OBJECT

public:
    static Hardcover *instance();
    // enabled reads Hardcover in the [NickelSync] group of config.ini.
    static bool enabled();
    // addButton adds the "Hardcover" button to a row of the NickelSync page
    // and returns it. screen is the page size.
    QLabel *addButton(QBoxLayout *row, QSize screen);

private Q_SLOTS:
    void onButton();
    void onPoll();
    void load();

private:
    enum Auth { SignedOut, SigningIn, SignedIn };
    enum Load { NotLoaded, Loading, Loaded, NotFound, Failed };

    struct Book {
        QString contentId;
        QString title;
        QString author;
    };

    struct Status {
        // The book on Hardcover.
        QString bookTitle;
        QString subtitle;
        QString authors;
        QString series; // the featured series and the position, "Name #2"
        QString genres;
        QString description;
        int year = 0;
        int bookPages = 0;
        double rating = 0; // the average of all users
        int ratings = 0;
        int readers = 0; // users that read the book
        // The book in the library of the user.
        bool inLibrary = false;
        QString status;
        double progress = -1;
        int pages = 0;
        int editionPages = 0;
        double userRating = 0;
        int readCount = 0;
        QDate started;
        QDate finished;
        QDate updated;
    };

    // CacheState tells if a page shows data from the cache, and how the
    // update of that data goes.
    struct CacheState {
        QDateTime saved; // when the data was saved. Invalid: not from the cache.
        bool updating = false;
        QString error; // why the update failed
    };

    // A journal page shows the entries that fit the screen, so it gets more
    // entries than one page can show.
    static const int JOURNAL_FETCH = 15;
    static const int SEARCH_PAGE = 6;  // results on a search page

    typedef std::function<void(int status, QJsonObject const &body)> Reply;
    typedef std::function<void(QJsonObject const &data, QString const &error)> Result;

    explicit Hardcover(QObject *parent = nullptr);

    // hardcover.cc
    bool wifiConnected();
    void post(QString const &url, QByteArray const &body, bool json, bool withToken, Reply done);
    void graphql(char const *query, QJsonObject const &vars, Result done, bool retried = false);
    void refresh(std::function<void(bool ok)> done);
    QString refreshError() const;
    void saveTokens(QJsonObject const &body);
    void clearTokens();
    bool autoLoad();
    void startSignIn();
    void cancelSignIn();
    void signOut();
    void loadStatus(int bookId);
    void applyStatus(int bookId, QJsonObject const &data);
    void loadFailed(QString const &error);
    void loadJournal();
    void search();
    void link(int bookId);
    void unlink();
    Book openBook();
    QString linkKey() const;
    int linkedBook() const;

    // hardcoverpage.cc
    QLabel *label(QLayout *layout, char const *name, QString const &text);
    QLabel *button(QLayout *layout, QString const &text, char const *action, bool primary);
    int px(int at1264) const;
    QString style() const;
    static QString cacheText(CacheState const &c);
    void updateJournalNote();
    QWidget *newPage(QString const &title, QWidget *&content, bool fill = false);
    QVBoxLayout *freshBody(QWidget *container, QPointer<QWidget> &body);
    void rebuild();
    void rebuildJournal();
    void rebuildSearch();
    void showPage();
    void showJournal();
    void openJournal();
    void showSearch();

    Auth auth = SignedOut;
    Load loadState = NotLoaded;
    QString message; // an error or a note for the Hardcover page
    Book book;
    int bookId = 0;
    Status status;
    CacheState statusCache;

    QString deviceCode;
    QString userCode;
    QString verifyUrl;
    int pollSeconds = 5;
    QDateTime pollDeadline;
    QTimer poll;

    bool refreshing = false;
    QList<std::function<void(bool)>> refreshWaiters;

    QJsonArray journal;
    int journalOffset = 0;
    QList<int> journalBack; // the offsets of the newer pages
    int journalShown = 0;   // the entries on the page
    bool journalMore = false;
    bool journalLoading = false;
    int journalGen = 0; // counts the journal requests; a reply of an older one is dropped
    QString journalError;
    CacheState journalCache;
    QPointer<QLabel> journalNote; // the cache line of the journal page

    QJsonArray results;
    int searchPageNo = 1;
    bool searchMore = false;
    bool searchLoading = false;
    QString searchError;

    QSize screen;
    QNetworkAccessManager *net = nullptr;
    QPointer<QWidget> mainView;    // the Hardcover page
    QPointer<QWidget> mainContent; // its content widget
    QPointer<QWidget> mainBody;    // the content, built again on each change
    QPointer<QWidget> journalView;    // the journal page
    QPointer<QWidget> journalContent; // its content widget
    QPointer<QWidget> journalBody;
    QPointer<QWidget> searchView;     // the "Link book" page
    QPointer<QWidget> searchContent;
    QPointer<QWidget> searchBody;
};

#endif
