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
    // addButton adds the "Hardcover" button to a row of the NickelSync page.
    // screen is the page size.
    void addButton(QBoxLayout *row, QSize screen);

private Q_SLOTS:
    void onButton();
    void onPoll();

private:
    enum Auth { SignedOut, SigningIn, SignedIn };
    enum Load { NotLoaded, Loading, Loaded, NotFound, Failed };

    struct Book {
        QString contentId;
        QString title;
        QString author;
    };

    struct Status {
        bool inLibrary = false;
        QString bookTitle;
        QString status;
        double progress = -1;
        int pages = 0;
        QDate started;
        QDate finished;
        QDate updated;
    };

    static const int JOURNAL_PAGE = 5; // entries on a journal page
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
    void startSignIn();
    void cancelSignIn();
    void signOut();
    void load();
    void loadStatus(int bookId);
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
    QWidget *newPage(QString const &title, QWidget *&content);
    QVBoxLayout *freshBody(QWidget *container, QPointer<QWidget> &body);
    void rebuild();
    void rebuildJournal();
    void rebuildSearch();
    void showPage();
    void showJournal();
    void showSearch();

    Auth auth = SignedOut;
    Load loadState = NotLoaded;
    QString message; // an error or a note for the Hardcover page
    Book book;
    int bookId = 0;
    Status status;

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
    bool journalMore = false;
    bool journalLoading = false;
    QString journalError;

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
