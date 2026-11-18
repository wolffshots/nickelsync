// Hardcover: the button on the NickelSync page, the Hardcover page, the reading
// journal page and the "Link book" page. The pages are N3Dialog pages, as the
// NickelSync page.
// Each one has a body widget that rebuild*() builds again when its data
// changes. The sign-in and the queries are in hardcover.cc.

#include <QFrame>
#include <QHBoxLayout>
#include <QJsonValue>
#include <QLabel>
#include <QLocale>
#include <QVBoxLayout>
#include <QVariant>

#include <cstdlib>

#include <NickelHook.h>

#include "hardcover.h"
#include "nickel.h"

static QString const DATE_FORMAT = QStringLiteral("d MMMM yyyy");

int Hardcover::px(int at1264) const { return at1264 * screen.width() / 1264; }

// style has the label styles of the NickelSync page, and the Hardcover ones.
QString Hardcover::style() const {
    return QStringLiteral("QLabel#nsKey { font-size: %1px; color: #666666; }"
                          "QLabel#nsValue { font-size: %2px; }"
                          "QLabel#nsDesc { font-size: %1px; }"
                          "QLabel#hcCode { font-size: %3px; font-weight: bold; }"
                          "QLabel#hcEntry { font-size: %1px; }"
                          "QLabel#hcHead { font-size: %1px; font-weight: bold; }"
                          "QFrame#hcBox { border: 1px solid #999999; }"
                          "QLabel#hcResult { font-size: %1px; border-bottom: 1px solid #999999; padding: %4px 0px; }")
        .arg(px(30))
        .arg(px(38))
        .arg(px(72))
        .arg(px(16));
}

QLabel *Hardcover::label(QLayout *layout, char const *name, QString const &text) {
    QLabel *l = new QLabel(text);
    l->setObjectName(QString::fromLatin1(name));
    l->setWordWrap(true);
    l->setTextFormat(Qt::PlainText);
    layout->addWidget(l);
    return l;
}

// button adds a Nickel button. Add a sub-layout to its parent layout first,
// so that the layout has a parent widget.
QLabel *Hardcover::button(QLayout *layout, QString const &text, char const *action, bool primary) {
    QLabel *b = reinterpret_cast<QLabel *>(calloc(1, 512));
    N3ButtonLabel__constructor(b, layout->parentWidget());
    // On a dark page (followDarkMode), a primary button is hard to see, so it
    // is a bordered button.
    for (QWidget *w = layout->parentWidget(); primary && w; w = w->parentWidget())
        if (w->property("darkMode").toBool())
            primary = false;
    // Without a style property, Nickel draws the button as italic text.
    b->setProperty(primary ? "primaryButton" : "borderedButton", true);
    b->setProperty("hcAction", QByteArray(action));
    b->setText(text);
    connect(b, SIGNAL(tapped(bool)), this, SLOT(onButton()));
    layout->addWidget(b);
    if (QBoxLayout *box = qobject_cast<QBoxLayout *>(layout))
        box->setAlignment(b, Qt::AlignLeft);
    return b;
}

static QHBoxLayout *row(QVBoxLayout *parent, int spacing) {
    QHBoxLayout *r = new QHBoxLayout();
    r->setSpacing(spacing);
    parent->addLayout(r);
    return r;
}

// freshBody replaces the body widget of a container with an empty one. The
// old body can hold the button that was tapped, so delete it later. The new
// body stays hidden while the caller fills it, and ShowAtEnd shows it. So the
// hide of the old body and the new body show in one paint, which is one e-ink
// update. A child that a layout adds to a visible widget shows only in the
// next event loop pass, which is a second paint.
QVBoxLayout *Hardcover::freshBody(QWidget *container, QPointer<QWidget> &body) {
    if (body) {
        body->hide();
        body->deleteLater();
    }
    body = new QWidget(container);
    QVBoxLayout *l = new QVBoxLayout(body);
    l->setContentsMargins(0, 0, 0, 0);
    l->setSpacing(px(12));
    body->hide();
    static_cast<QVBoxLayout *>(container->layout())->insertWidget(0, body);
    return l;
}

namespace {
// ShowAtEnd shows a body that freshBody made when a rebuild function returns.
struct ShowAtEnd {
    QPointer<QWidget> &body;
    ~ShowAtEnd() {
        if (body)
            body->show();
    }
};
} // namespace

// addButton also gets the open book. The NickelSync page hides the book, so
// the Hardcover page cannot get it later.
QLabel *Hardcover::addButton(QBoxLayout *row, QSize size) {
    screen = size;
    Book open = openBook();
    if (open.contentId != book.contentId) {
        loadState = NotLoaded;
        bookId = 0;
        message.clear();
    }
    book = open;
    return button(row, QStringLiteral("Hardcover"), "open", false);
}

// showPage opens the Hardcover page over the NickelSync page.
void Hardcover::showPage() {
    if (mainView)
        return;
    QWidget *content = nullptr;
    mainView = newPage(QStringLiteral("Hardcover"), content);
    mainContent = content;
    if (!autoLoad())
        rebuild();
    openSubPage(mainView);
}

// rebuild builds the Hardcover page again from the state.
void Hardcover::rebuild() {
    if (!mainContent)
        return;
    QVBoxLayout *l = freshBody(mainContent, mainBody);
    ShowAtEnd show{mainBody};

    if (auth == SignedOut) {
        label(l, "nsDesc", QStringLiteral("Sign in to show the Hardcover status and reading journal of this book."));
        if (!message.isEmpty())
            label(l, "nsDesc", message);
        button(row(l, px(24)), QStringLiteral("Sign in to Hardcover"), "signin", true);
        return;
    }

    if (auth == SigningIn) {
        if (userCode.isEmpty()) {
            label(l, "nsDesc", QStringLiteral("Asking Hardcover for a code"));
        } else {
            QString host = verifyUrl;
            host.remove(QStringLiteral("https://"));
            label(l, "nsDesc", QStringLiteral("On your phone or computer, go to %1 and enter this code:").arg(host));
            label(l, "hcCode", userCode);
            label(l, "nsDesc", QStringLiteral("Waiting for approval"));
        }
        button(row(l, px(24)), QStringLiteral("Cancel"), "cancel", false);
        return;
    }

    if (book.contentId.isEmpty()) {
        label(l, "nsDesc", QStringLiteral("Open a book to show its Hardcover data."));
    } else if (loadState == Loading) {
        label(l, "nsDesc", QStringLiteral("Loading from Hardcover"));
    } else if (loadState == NotFound) {
        label(l, "nsDesc", QStringLiteral("This book is not on Hardcover, or it has no ISBN. Link it to a Hardcover book."));
    } else if (loadState == Loaded) {
        // The book on Hardcover.
        QLocale en(QLocale::English, QLocale::UnitedStates);
        label(l, "nsValue", status.bookTitle.isEmpty() ? QStringLiteral("Hardcover book %1").arg(bookId) : status.bookTitle);
        if (!status.subtitle.isEmpty())
            label(l, "nsDesc", status.subtitle);
        if (!status.authors.isEmpty())
            label(l, "nsDesc", QStringLiteral("by ") + status.authors);
        QStringList facts;
        if (!status.series.isEmpty())
            facts << status.series;
        if (status.year > 0)
            facts << QStringLiteral("Published %1").arg(status.year);
        if (status.bookPages > 0)
            facts << QStringLiteral("%1 pages").arg(status.bookPages);
        if (status.rating > 0)
            facts << QStringLiteral("Rating %1 (%2 ratings)").arg(QString::number(status.rating, 'f', 1), en.toString(status.ratings));
        if (status.readers > 0)
            facts << QStringLiteral("Read by %1").arg(en.toString(status.readers));
        if (!facts.isEmpty())
            label(l, "nsKey", facts.join(QStringLiteral(". ")));
        if (!status.genres.isEmpty())
            label(l, "nsKey", status.genres);
        if (!status.description.isEmpty())
            label(l, "hcEntry", status.description);
        l->addSpacing(px(24));

        // The book in the library of the user.
        if (!status.inLibrary) {
            label(l, "nsDesc", QStringLiteral("Not in your Hardcover library"));
        } else {
            QString line = status.status.isEmpty() ? QStringLiteral("In your library") : status.status;
            if (status.progress >= 0)
                line += QStringLiteral(", %1%").arg(qRound(status.progress));
            if (status.pages > 0)
                line += status.editionPages > 0 ? QStringLiteral(", page %1 of %2").arg(status.pages).arg(status.editionPages)
                                                : QStringLiteral(", page %1").arg(status.pages);
            label(l, "nsDesc", line);
            QStringList mine;
            if (status.userRating > 0)
                mine << QStringLiteral("Your rating ") + QString::number(status.userRating);
            if (status.readCount > 1)
                mine << QStringLiteral("Read %1 times").arg(status.readCount);
            if (status.started.isValid())
                mine << QStringLiteral("Started ") + status.started.toString(DATE_FORMAT);
            if (status.finished.isValid())
                mine << QStringLiteral("Finished ") + status.finished.toString(DATE_FORMAT);
            if (status.updated.isValid())
                mine << QStringLiteral("Updated ") + status.updated.toString(DATE_FORMAT);
            if (!mine.isEmpty())
                label(l, "nsKey", mine.join(QStringLiteral(". ")));
        }
    }
    if (!message.isEmpty())
        label(l, "nsDesc", message);

    QHBoxLayout *r = row(l, px(24));
    if (!book.contentId.isEmpty() && loadState != Loading) {
        if (loadState == Loaded)
            button(r, QStringLiteral("Reading journal"), "journal", true);
        else if (loadState != NotFound)
            button(r, QStringLiteral("Load from Hardcover"), "load", true);
        if (linkedBook() > 0)
            button(r, QStringLiteral("Unlink book"), "unlink", false);
        else if (loadState == NotFound)
            button(r, QStringLiteral("Link book"), "link", true);
    }
    button(r, QStringLiteral("Sign out"), "signout", false);
    r->addStretch(1);
}

void Hardcover::onButton() {
    QObject *b = sender();
    if (!b)
        return;
    QByteArray a = b->property("hcAction").toByteArray();
    nh_log("NickelSync: HC: TAP: %s", a.constData());
    if (a == "open") {
        showPage();
    } else if (a == "signin") {
        startSignIn();
    } else if (a == "cancel") {
        cancelSignIn();
    } else if (a == "signout") {
        signOut();
    } else if (a == "load") {
        load();
    } else if (a == "journal") {
        showJournal();
    } else if (a == "link") {
        showSearch();
    } else if (a == "unlink") {
        unlink();
    } else if ((a == "newer" || a == "older" || a == "reload") && journalLoading) {
        // The entries stay on the page while the next page loads.
    } else if ((a == "previous" || a == "more") && searchLoading) {
        // The results stay on the page while the next page loads.
    } else if (a == "newer") {
        journalOffset = journalBack.isEmpty() ? 0 : journalBack.takeLast();
        loadJournal();
    } else if (a == "older") {
        journalBack.append(journalOffset);
        journalOffset += journalShown;
        loadJournal();
    } else if (a == "reload") {
        loadJournal();
    } else if (a == "previous") {
        searchPageNo = qMax(1, searchPageNo - 1);
        search();
    } else if (a == "more") {
        searchPageNo++;
        search();
    } else if (a == "pick") {
        link(b->property("hcBook").toInt());
    }
}

// newPage makes a full-screen page, as showPage() in nickelsync.cc does.
// content gets a layout with a stretch at the end, or, with fill, no stretch:
// then the body fills the page. Fill content, then open the page with
// openSubPage(), so that the first paint shows the content: each later paint
// is one more e-ink update.
QWidget *Hardcover::newPage(QString const &title, QWidget *&content, bool fill) {
    content = new QWidget();
    content->setStyleSheet(style());
    QVBoxLayout *l = new QVBoxLayout(content);
    l->setContentsMargins(px(75), px(40), px(75), px(40));
    if (!fill)
        l->addStretch(1);
    QWidget *page = N3DialogFactory__getDialog(content, true);
    N3Dialog__setTitle(page, title);
    page->setFixedSize(screen);
    followDarkMode(page, content);
    return page;
}

// ---- reading journal -----------------------------------------------------

// plainText gets the text of a review. Hardcover keeps a review as Slate
// nodes: objects with "text", or "children" and a "type".
static QString plainText(QJsonValue const &v) {
    if (v.isString())
        return v.toString();
    QString out;
    if (v.isArray()) {
        for (QJsonValue const &c : v.toArray())
            out += plainText(c);
    } else if (v.isObject()) {
        QJsonObject o = v.toObject();
        out +=o.contains(QStringLiteral("text")) ? o.value(QStringLiteral("text")).toString()
                                                  : plainText(o.value(QStringLiteral("children")));
        if (o.value(QStringLiteral("type")).toString() == QLatin1String("paragraph"))
            out += QLatin1Char('\n');
    }
    return out;
}

// describe gives the heading and the text of a journal entry.
static void describe(QJsonObject const &e, QString &heading, QString &text) {
    QString event = e.value(QStringLiteral("event")).toString();
    QJsonObject meta = e.value(QStringLiteral("metadata")).toObject();
    text = e.value(QStringLiteral("entry")).toString();
    if (event == QLatin1String("quote")) {
        heading = QStringLiteral("Quote");
    } else if (event == QLatin1String("note")) {
        heading = QStringLiteral("Note");
    } else if (event == QLatin1String("reviewed")) {
        heading = QStringLiteral("Review");
        text = plainText(meta.value(QStringLiteral("review"))).trimmed();
    } else if (event == QLatin1String("rated")) {
        heading = QStringLiteral("Rated ") + meta.value(QStringLiteral("rating")).toVariant().toString();
    } else if (event == QLatin1String("progress_updated")) {
        heading = QStringLiteral("Progress %1% to %2%")
                      .arg(meta.value(QStringLiteral("progress_was")).toVariant().toInt())
                      .arg(meta.value(QStringLiteral("progress")).toVariant().toInt());
    } else if (event == QLatin1String("status_want_to_read")) {
        heading = QStringLiteral("Want to Read");
    } else if (event == QLatin1String("user_book_read_started") || event == QLatin1String("status_currently_reading")) {
        heading = QStringLiteral("Started reading");
    } else if (event == QLatin1String("user_book_read_finished") || event == QLatin1String("status_read")) {
        heading = QStringLiteral("Finished reading");
    } else if (event == QLatin1String("status_stopped")) {
        heading = QStringLiteral("Stopped reading");
    } else if (event == QLatin1String("status_paused")) {
        heading = QStringLiteral("Paused reading");
    } else if (event == QLatin1String("list_book")) {
        heading = QStringLiteral("Added to the list ") + meta.value(QStringLiteral("list_name")).toString();
    } else if (event == QLatin1String("prompt_book")) {
        heading = QStringLiteral("Answered the prompt ") + meta.value(QStringLiteral("prompt")).toString();
    } else {
        heading = event;
        heading.replace(QLatin1Char('_'), QLatin1Char(' '));
    }
    // An e-ink page does not scroll. Keep each entry short.
    if (text.size() > 400)
        text = text.left(400).trimmed() + QStringLiteral("...");
}

// entryTime gives the time of a journal entry, such as
// 2026-11-15T09:12:00+00:00 (UTC), in local time, as koboTime in serverstats.cc.
static QString entryTime(QJsonValue const &v) {
    QDateTime t = QDateTime::fromString(v.toString().left(19), QStringLiteral("yyyy-MM-ddTHH:mm:ss"));
    if (!t.isValid())
        return QString();
    t.setTimeSpec(Qt::UTC);
    return t.toLocalTime().toString(QStringLiteral("d MMMM yyyy, HH:mm"));
}

// showJournal loads the first journal page. openJournal opens the page when
// the entries arrive.
void Hardcover::showJournal() {
    if (journalView || journalLoading || bookId <= 0)
        return;
    journalOffset = 0;
    journalBack.clear();
    journalShown = 0;
    journal = QJsonArray();
    loadJournal();
}

// openJournal builds the journal page again, or opens it over the Hardcover
// page. If the user closed the Hardcover page in the meantime, it does nothing.
void Hardcover::openJournal() {
    if (journalView) {
        rebuildJournal();
        return;
    }
    if (!mainView)
        return;
    QWidget *content = nullptr;
    journalView = newPage(QStringLiteral("Reading journal"), content, true);
    journalContent = content;
    rebuildJournal();
    openSubPage(journalView);
}

// rebuildJournal shows the entries from journalOffset on that fit the page,
// each in a box. The pager row is at the bottom of the page, and Newer and
// Older keep their place, also when one of them is hidden.
void Hardcover::rebuildJournal() {
    if (!journalContent)
        return;
    QVBoxLayout *l = freshBody(journalContent, journalBody);
    ShowAtEnd show{journalBody};
    l->setSpacing(px(16));
    QLabel *title = label(l, "nsValue", status.bookTitle);
    if (!journalError.isEmpty()) {
        label(l, "nsDesc", journalError);
        button(row(l, px(24)), QStringLiteral("Try again"), "reload", true);
    } else if (journal.isEmpty()) {
        label(l, "nsDesc", QStringLiteral("No journal entries for this book."));
    }
    if (!journalError.isEmpty() || journal.isEmpty()) {
        l->addStretch(1);
        return;
    }

    // Make the pager row first, to know its height.
    QWidget *pager = new QWidget(journalBody);
    QHBoxLayout *pr = new QHBoxLayout(pager);
    pr->setContentsMargins(0, 0, 0, 0);
    QLabel *newer = button(pr, QStringLiteral("Newer"), "newer", false);
    pr->addStretch(1);
    QLabel *range = label(pr, "nsKey", QString());
    range->setWordWrap(false);
    pr->addStretch(1);
    QLabel *older = button(pr, QStringLiteral("Older"), "older", false);
    // The same width for both, so the range stays in the middle.
    pager->ensurePolished();
    int bw = qMax(newer->sizeHint().width(), older->sizeHint().width());
    for (QLabel *b : {newer, older}) {
        b->setMinimumWidth(bw);
        QSizePolicy sp = b->sizePolicy();
        sp.setRetainSizeWhenHidden(true);
        b->setSizePolicy(sp);
    }

    // The first build is before the page shows. The Hardcover page below has
    // the same layout and shows, so use its size. Without it, guess.
    QWidget *ref = journalContent->isVisible() ? journalContent.data() : mainContent.data();
    int width = ref ? ref->width() - px(150) : 0;
    int height = ref ? ref->height() - px(80) : 0;
    if (width <= 0 || height <= 0) {
        width = screen.width() - px(150);
        height = screen.height() * 3 / 4;
    }
    journalBody->ensurePolished();
    int avail = height - title->heightForWidth(width) - pager->sizeHint().height() - 2 * l->spacing();

    int used = 0;
    journalShown = 0;
    for (QJsonValue const &v : journal) {
        QJsonObject e = v.toObject();
        QString heading, text;
        describe(e, heading, text);
        QFrame *box = new QFrame(journalBody);
        box->setObjectName(QStringLiteral("hcBox"));
        QVBoxLayout *bl = new QVBoxLayout(box);
        bl->setContentsMargins(px(24), px(16), px(24), px(16));
        bl->setSpacing(px(8));
        QHBoxLayout *head = new QHBoxLayout();
        bl->addLayout(head);
        label(head, "hcHead", heading);
        QLabel *when = label(head, "nsKey", entryTime(e.value(QStringLiteral("action_at"))));
        when->setWordWrap(false);
        when->setAlignment(Qt::AlignRight | Qt::AlignTop);
        if (!text.isEmpty())
            label(bl, "hcEntry", text);
        box->ensurePolished();
        int h = box->heightForWidth(width);
        if (h < 0)
            h = box->sizeHint().height();
        // Show at least one entry, also if it does not fit.
        if (journalShown > 0 && used + h > avail) {
            delete box;
            break;
        }
        l->addWidget(box);
        used += h + l->spacing();
        journalShown++;
    }
    journalMore = journal.size() > journalShown;
    if (journalOffset == 0)
        newer->hide();
    if (!journalMore)
        older->hide();
    range->setText(QStringLiteral("Entries %1 to %2").arg(journalOffset + 1).arg(journalOffset + journalShown));
    l->addStretch(1);
    l->addWidget(pager);
}

// ---- link book -------------------------------------------------------------

void Hardcover::showSearch() {
    if (searchView || book.contentId.isEmpty())
        return;
    searchPageNo = 1;
    results = QJsonArray();
    QWidget *content = nullptr;
    searchView = newPage(QStringLiteral("Link book"), content);
    searchContent = content;
    search();
    openSubPage(searchView);
}

// authorsOf gets the author names of a search result.
static QString authorsOf(QJsonObject const &doc) {
    QStringList names;
    for (QJsonValue const &n : doc.value(QStringLiteral("author_names")).toArray())
        names << n.toString();
    if (names.isEmpty()) {
        for (QJsonValue const &c : doc.value(QStringLiteral("contributions")).toArray()) {
            QJsonObject o = c.toObject();
            if (o.value(QStringLiteral("contribution")).isNull())
                names << o.value(QStringLiteral("author")).toObject().value(QStringLiteral("name")).toString();
        }
    }
    names.removeAll(QString());
    return names.join(QStringLiteral(", "));
}

void Hardcover::rebuildSearch() {
    if (!searchContent)
        return;
    QVBoxLayout *l = freshBody(searchContent, searchBody);
    ShowAtEnd show{searchBody};
    label(l, "nsDesc", QStringLiteral("Tap the Hardcover book for \"%1\".").arg(book.title));
    l->addSpacing(px(16));
    if (searchLoading) {
        label(l, "nsDesc", QStringLiteral("Searching Hardcover"));
        return;
    }
    if (!searchError.isEmpty())
        label(l, "nsDesc", searchError);
    else if (results.isEmpty())
        label(l, "nsDesc", QStringLiteral("No results on Hardcover."));
    for (QJsonValue const &v : results) {
        QJsonObject doc = v.toObject();
        QJsonValue id = doc.value(QStringLiteral("id"));
        int bid = id.isString() ? id.toString().toInt() : id.toVariant().toInt();
        QString line = doc.value(QStringLiteral("title")).toString();
        QString authors = authorsOf(doc);
        if (!authors.isEmpty())
            line += QLatin1Char('\n') + authors;
        int year = doc.value(QStringLiteral("release_year")).toVariant().toInt();
        if (year > 0)
            line += QStringLiteral(", %1").arg(year);
        QLabel *t = reinterpret_cast<QLabel *>(calloc(1, 128));
        TouchLabel__constructor(t, searchBody, 0);
        t->setObjectName(QStringLiteral("hcResult"));
        t->setTextFormat(Qt::PlainText);
        t->setWordWrap(true);
        t->setText(line);
        t->setProperty("hcAction", QByteArray("pick"));
        t->setProperty("hcBook", bid);
        connect(t, SIGNAL(tapped(bool)), this, SLOT(onButton()));
        l->addWidget(t);
    }
    l->addSpacing(px(24));
    QHBoxLayout *r = row(l, px(24));
    if (searchPageNo > 1)
        button(r, QStringLiteral("Previous"), "previous", false);
    if (searchMore)
        button(r, QStringLiteral("More results"), "more", false);
    r->addStretch(1);
}
