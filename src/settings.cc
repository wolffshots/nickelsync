// NickelSync settings: read config.ini, and the settings page that writes it.
//
// config.ini is the source of truth. kpm and the user can also edit it. So a
// write changes only the value of one key line. Comments, blank lines, the
// order, unknown keys, other sections and the line endings stay the same. kpm
// edits ini files in the same way (internal/modconfig in kpm).
//
// "Does not parse" means one of these:
//   - the file is larger than 256 KiB (the limit of kpm),
//   - the file has a NUL byte,
//   - a line is not blank, not a comment (first character ; or #), not a
//     section header ([name]) and not a key line (key=value, key not empty).
// Before a write, a file that does not parse becomes config.ini.corrupt, and
// NickelSync writes a new file from the template. A value that is not valid,
// for example QuickSync=maybe, is not a parse error: it gives the default,
// and a write replaces it.
//
// The new file is the config template of the kobo-registry entry, made from
// the settings table below. Keep the two the same.

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QHBoxLayout>
#include <QLabel>
#include <QList>
#include <QSaveFile>
#include <QScreen>
#include <QVBoxLayout>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <NickelHook.h>

#include "nickel.h"
#include "settings.h"

// The same paths as NS_DIR and NS_CONFIG in nickelsync.cc.
#define NS_DIR      "/mnt/onboard/.adds/nickelsync"
#define NS_CONFIG   NS_DIR "/config.ini"
#define NS_CORRUPT  NS_CONFIG ".corrupt"
#define NS_SECTION  "NickelSync"

static const int MAX_SIZE = 256 * 1024;

// The page uses the Nickel functions in nickel.h that nickelsync.cc resolves:
// MainWindowController__sharedInstance, MainWindowController__pushView,
// N3ButtonLabel__constructor, N3DialogFactory__getDialog and
// N3Dialog__setTitle. If one is missing, nickelsync.cc shows no icon, so no
// page opens.

namespace {

enum Type { Bool, Int };

struct Setting {
    char const *key;
    Type type;
    char const *def;     // the default value
    int min, max;        // Int: the valid range. A value outside it is clamped.
    char const *steps;   // Int: the values that a tap goes through
    char const *unit;    // Int: the text after the value on the page
    char const *label;   // the name on the page
    char const *comment; // the comment above the key in a new file, and the text on the page
};

// To add a setting, add one line. The order is the order in a new file and on the page.
// clang-format off
Setting const settings[] = {
    {"ConnectTimeout",  Int,  "60",    10, 180, "30,60,90,120,180", " s",    "Wi-Fi timeout",        "Seconds to wait for a Wi-Fi connection (10 to 180)."},
    {"RestoreWifi",     Bool, "true",  0,  0,   nullptr,            nullptr, "Turn Wi-Fi off again", "If Wi-Fi was off before the sync, turn it off again after the sync."},
    {"QuickSync",       Bool, "false", 0,  0,   nullptr,            nullptr, "Quick sync",           "Inside a book, sync only the reading position and skip the full sync."},
    {"AutoSyncOnOpen",  Bool, "false", 0,  0,   nullptr,            nullptr, "Sync on book open",    "Sync the reading position when a book loads."},
    {"AutoSyncOnSleep", Bool, "false", 0,  0,   nullptr,            nullptr, "Sync on sleep",        "Sync when the device goes to sleep."},
    {"Hardcover",       Bool, "false", 0,  0,   nullptr,            nullptr, "Hardcover section",    "Show the Hardcover section on the NickelSync page."},
};
// clang-format on

Setting const *findSetting(QByteArray const &key) {
    for (Setting const &s : settings)
        if (key == s.key)
            return &s;
    nh_log("NickelSync: SETTINGS: no setting %s", key.constData());
    return nullptr;
}

// Line is one physical line of the file. Joining text + eol of each line
// gives the original bytes.
struct Line {
    QByteArray text;
    QByteArray eol; // "\n", "\r\n", or empty on a last line without a newline
};

enum Kind { Blank, Comment, Section, Key, Bad };

// classify gives the kind of a line. For a section header, name is the
// section. For a key line, name is the key and value the value, without one
// pair of double quotes (QSettings also removes them).
Kind classify(QByteArray text, QByteArray *name = nullptr, QByteArray *value = nullptr) {
    if (text.startsWith("\xEF\xBB\xBF")) // a UTF-8 byte order mark
        text = text.mid(3);
    QByteArray t = text.trimmed();
    if (t.isEmpty())
        return Blank;
    if (t.startsWith(';') || t.startsWith('#'))
        return Comment;
    if (t.startsWith('[') && t.endsWith(']')) {
        if (name)
            *name = t.mid(1, t.size() - 2).trimmed();
        return Section;
    }
    int eq = t.indexOf('=');
    if (eq <= 0 || t.left(eq).trimmed().isEmpty())
        return Bad;
    if (name)
        *name = t.left(eq).trimmed();
    if (value) {
        *value = t.mid(eq + 1).trimmed();
        if (value->size() >= 2 && value->startsWith('"') && value->endsWith('"'))
            *value = value->mid(1, value->size() - 2);
    }
    return Key;
}

struct Config {
    enum Status { Missing, Ok, Corrupt, Unreadable } status = Missing;
    QList<Line> lines;
    QString error; // why the file is Corrupt or Unreadable
};

Config readConfig() {
    Config c;
    QFile f(QStringLiteral(NS_CONFIG));
    if (!f.exists())
        return c;
    if (!f.open(QIODevice::ReadOnly)) {
        c.status = Config::Unreadable;
        c.error = f.errorString();
        return c;
    }
    QByteArray raw = f.read(MAX_SIZE + 1);
    if (f.error() != QFile::NoError) {
        c.status = Config::Unreadable;
        c.error = f.errorString();
        return c;
    }
    c.status = Config::Corrupt;
    if (raw.size() > MAX_SIZE) {
        c.error = QStringLiteral("the file is larger than 256 KiB");
        return c;
    }
    if (raw.contains('\0')) {
        c.error = QStringLiteral("the file has a NUL byte");
        return c;
    }
    for (int start = 0; start < raw.size();) {
        int nl = raw.indexOf('\n', start);
        int end = nl < 0 ? raw.size() : nl;
        Line ln{raw.mid(start, end - start), QByteArray()};
        if (nl >= 0) {
            ln.eol = "\n";
            if (ln.text.endsWith('\r')) {
                ln.text.chop(1);
                ln.eol = "\r\n";
            }
        }
        if (classify(ln.text) == Bad) {
            c.error = QStringLiteral("line %1 is not a section, a key or a comment").arg(c.lines.size() + 1);
            c.lines.clear();
            return c;
        }
        c.lines << ln;
        start = end + 1;
    }
    c.status = Config::Ok;
    return c;
}

// normalize gives the value in its written form, or an empty value if it is
// not valid.
QByteArray normalize(Setting const &s, QByteArray const &value) {
    if (s.type == Bool) {
        QByteArray v = value.toLower();
        if (v == "true" || v == "1")
            return "true";
        if (v == "false" || v == "0")
            return "false";
        return QByteArray();
    }
    bool ok = false;
    int n = value.toInt(&ok);
    return ok ? QByteArray::number(qBound(s.min, n, s.max)) : QByteArray();
}

// valueOf gives the value of s in c. The last key line in the [NickelSync]
// sections wins, as in QSettings.
QByteArray valueOf(Config const &c, Setting const &s) {
    QByteArray found;
    if (c.status == Config::Ok) {
        bool in = false;
        for (Line const &ln : c.lines) {
            QByteArray name, value;
            Kind k = classify(ln.text, &name, &value);
            if (k == Section)
                in = name == NS_SECTION;
            else if (in && k == Key && name == s.key)
                found = value;
        }
    }
    QByteArray v = normalize(s, found);
    return v.isEmpty() ? QByteArray(s.def) : v;
}

// setKey sets the value of s in lines. It changes only the value of each key
// line of s in a [NickelSync] section. It adds the comment and the key at the
// end of the last [NickelSync] section if the key is missing, and adds the
// section at the end of the file if the section is missing.
void setKey(QList<Line> &lines, Setting const &s, QByteArray const &value) {
    QByteArray eol = "\n";
    for (Line const &ln : lines) {
        if (!ln.eol.isEmpty()) {
            eol = ln.eol;
            break;
        }
    }
    bool in = false, done = false;
    int last = -1; // the last line of the last [NickelSync] section that is not blank
    for (int i = 0; i < lines.size(); i++) {
        QByteArray name;
        Kind k = classify(lines[i].text, &name);
        if (k == Section)
            in = name == NS_SECTION;
        if (!in || k == Blank)
            continue;
        last = i;
        if (k == Key && name == s.key) {
            // Keep the spaces around the old value, as kpm does.
            QByteArray &text = lines[i].text;
            int eq = text.indexOf('=');
            QByteArray rest = text.mid(eq + 1);
            QByteArray trimmed = rest.trimmed();
            int lead = trimmed.isEmpty() ? rest.size() : rest.indexOf(trimmed);
            int trail = trimmed.isEmpty() ? 0 : rest.size() - lead - trimmed.size();
            text = text.left(eq + 1) + rest.left(lead) + value + rest.right(trail);
            done = true;
        }
    }
    if (done)
        return;
    QList<Line> add;
    add << Line{QByteArray("; ") + s.comment, eol} << Line{QByteArray(s.key) + "=" + value, eol};
    if (last < 0)
        add.prepend(Line{"[" NS_SECTION "]", eol});
    int at = last < 0 ? lines.size() : last + 1;
    // The line before the new lines needs a line end.
    if (at > 0 && lines[at - 1].eol.isEmpty())
        lines[at - 1].eol = eol;
    for (int i = 0; i < add.size(); i++)
        lines.insert(at + i, add[i]);
}

// templateLines gives a new file with the default values.
QList<Line> templateLines() {
    QList<Line> lines;
    lines << Line{"[" NS_SECTION "]", "\n"};
    for (Setting const &s : settings)
        lines << Line{QByteArray("; ") + s.comment, "\n"} << Line{QByteArray(s.key) + "=" + s.def, "\n"};
    return lines;
}

// writeSetting writes the value of s to config.ini. It returns an error text,
// or an empty text on success. reset is true if the old file did not parse
// and is now config.ini.corrupt.
QString writeSetting(Setting const &s, QByteArray const &value, bool *reset) {
    *reset = false;
    Config c = readConfig();
    if (c.status == Config::Unreadable)
        return QStringLiteral("Could not read config.ini: ") + c.error;
    if (c.status == Config::Corrupt) {
        // rename() replaces an old config.ini.corrupt.
        if (std::rename(NS_CONFIG, NS_CORRUPT) != 0)
            return QStringLiteral("Could not rename config.ini: ") + QString::fromLocal8Bit(std::strerror(errno));
        nh_log("NickelSync: SETTINGS: config.ini does not parse (%s), renamed to config.ini.corrupt", qPrintable(c.error));
        *reset = true;
    }
    QList<Line> lines = c.status == Config::Ok ? c.lines : templateLines();
    setKey(lines, s, value);
    QByteArray out;
    for (Line const &ln : lines)
        out += ln.text + ln.eol;

    // QSaveFile writes a temporary file in the same folder, then renames it
    // to config.ini. So a power loss leaves the old file or the new file.
    QDir().mkpath(QStringLiteral(NS_DIR));
    QSaveFile f(QStringLiteral(NS_CONFIG));
    if (!f.open(QIODevice::WriteOnly) || f.write(out) != out.size() || !f.commit())
        return QStringLiteral("Could not write config.ini: ") + f.errorString();
    nh_log("NickelSync: SETTINGS: %s=%s", s.key, value.constData());
    return QString();
}

// nextValue gives the value after value: the other boolean, or the next step.
QByteArray nextValue(Setting const &s, QByteArray const &value) {
    if (s.type == Bool)
        return value == "true" ? "false" : "true";
    QList<QByteArray> steps = QByteArray(s.steps).split(',');
    for (QByteArray const &step : steps)
        if (step.toInt() > value.toInt())
            return step;
    return steps.first();
}

QString display(Setting const &s, QByteArray const &value) {
    if (s.type == Bool)
        return value == "true" ? QStringLiteral("On") : QStringLiteral("Off");
    return QString::fromLatin1(value) + QString::fromLatin1(s.unit);
}

QLabel *newButton(QWidget *parent, QString const &text) {
    // The size that NickelHardcover allocates.
    QLabel *b = reinterpret_cast<QLabel *>(calloc(1, 512));
    N3ButtonLabel__constructor(b, parent);
    b->setText(text);
    return b;
}

QLabel *newLabel(char const *name, QString const &text) {
    QLabel *l = new QLabel(text);
    l->setObjectName(QString::fromLatin1(name));
    l->setWordWrap(true);
    return l;
}

} // namespace

bool settingBool(char const *key) {
    Setting const *s = findSetting(key);
    return s && valueOf(readConfig(), *s) == "true";
}

int settingInt(char const *key) {
    Setting const *s = findSetting(key);
    return s ? valueOf(readConfig(), *s).toInt() : 0;
}

void SettingsPage::addButton(QVBoxLayout *layout, QWidget *parent) {
    static SettingsPage *instance = new SettingsPage();
    QLabel *b = newButton(parent, QStringLiteral("Settings"));
    connect(b, SIGNAL(tapped(bool)), instance, SLOT(open()));
    layout->addWidget(b, 0, Qt::AlignLeft);
}

// open opens the settings page over the NickelSync page, in the same way as
// showPage() in nickelsync.cc.
void SettingsPage::open() {
    if (page)
        return;
    QWidget *from = qobject_cast<QWidget *>(sender());
    QSize screen = from ? from->window()->size() : QApplication::primaryScreen()->geometry().size();
    auto px = [screen](int at1264) { return at1264 * screen.width() / 1264; };

    QWidget *content = new QWidget();
    content->setStyleSheet(QStringLiteral("QLabel#nsName { font-size: %2px; }"
                                          "QLabel#nsDesc { font-size: %1px; }"
                                          "QLabel#nsNote { font-size: %1px; color: #666666; }")
                               .arg(px(30))
                               .arg(px(38)));
    QVBoxLayout *l = new QVBoxLayout(content);
    l->setContentsMargins(px(75), px(40), px(75), px(40));
    l->setSpacing(px(48));

    for (Setting const &s : settings) {
        QVBoxLayout *text = new QVBoxLayout();
        text->setSpacing(px(8));
        text->addWidget(newLabel("nsName", QString::fromUtf8(s.label)));
        text->addWidget(newLabel("nsDesc", QString::fromUtf8(s.comment)));
        QLabel *b = newButton(content, QString());
        b->setProperty("nsKey", QByteArray(s.key));
        b->setAlignment(Qt::AlignCenter);
        b->setMinimumWidth(px(220));
        connect(b, SIGNAL(tapped(bool)), this, SLOT(onTap()));
        QHBoxLayout *row = new QHBoxLayout();
        row->setSpacing(px(40));
        row->addLayout(text, 1);
        row->addWidget(b, 0, Qt::AlignVCenter);
        l->addLayout(row);
    }
    note = newLabel("nsNote", QString());
    l->addWidget(note);
    l->addStretch(1);

    page = N3DialogFactory__getDialog(content, true);
    N3Dialog__setTitle(page, QStringLiteral("NickelSync settings"));
    page->setFixedSize(screen);
    refresh();
    MainWindowController__pushView(MainWindowController__sharedInstance(), page);
    connect(page, SIGNAL(closeTapped()), page, SLOT(deleteLater()));
    page->show();
    nh_log("NickelSync: SETTINGS: page opened");
}

// refresh reads config.ini again and shows its values. message replaces the
// note below the settings.
void SettingsPage::refresh(QString const &message) {
    if (!page)
        return;
    Config c = readConfig();
    for (QLabel *b : page->findChildren<QLabel *>()) {
        QByteArray key = b->property("nsKey").toByteArray();
        if (Setting const *s = key.isEmpty() ? nullptr : findSetting(key))
            b->setText(display(*s, valueOf(c, *s)));
    }
    QString text = message;
    if (text.isEmpty()) {
        switch (c.status) {
        case Config::Missing:
            text = QStringLiteral("No config.ini yet. A change writes it.");
            break;
        case Config::Corrupt:
            text = QStringLiteral("config.ini does not parse: %1. The page shows the defaults. "
                                  "A change renames the file to config.ini.corrupt and writes a new config.ini.")
                       .arg(c.error);
            break;
        case Config::Unreadable:
            text = QStringLiteral("Could not read config.ini: ") + c.error;
            break;
        case Config::Ok:
            text = QStringLiteral("A change is saved in .adds/nickelsync/config.ini at once.");
            break;
        }
    }
    note->setText(text);
}

void SettingsPage::onTap() {
    QObject *b = sender();
    Setting const *s = b ? findSetting(b->property("nsKey").toByteArray()) : nullptr;
    if (!s)
        return;
    bool reset = false;
    QString error = writeSetting(*s, nextValue(*s, valueOf(readConfig(), *s)), &reset);
    if (!error.isEmpty())
        nh_log("NickelSync: SETTINGS: %s", qPrintable(error));
    refresh(!error.isEmpty() ? error
            : reset          ? QStringLiteral("config.ini did not parse. The old file is now config.ini.corrupt.")
                             : QString());
}
