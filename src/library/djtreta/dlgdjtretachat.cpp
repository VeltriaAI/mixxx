#include "library/djtreta/dlgdjtretachat.h"

#include <algorithm>
#include <utility>
#include <vector>

#include <QCompleter>
#include <QDateTime>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QScrollBar>
#include <QShortcut>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QTabBar>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWebSocket>

#include "moc_dlgdjtretachat.cpp"

namespace {
const QString kBase = QStringLiteral("http://127.0.0.1:7779");
constexpr int kPollMs = 1500;

struct SlashCmd {
    const char* name;
    const char* desc;
};
const SlashCmd kSlashCmds[] = {
        {"/auto", "Autonomous mode — Treta drives the set"},
        {"/sarathi", "Sarathi mode — you drive on the FLX4"},
        {"/skip", "Skip the current track"},
        {"/mood", "Change mood / genre   (e.g. /mood deep)"},
        {"/accept", "Fire Treta's transition suggestion"},
        {"/reject", "Drop her suggestion + reshape"},
        {"/like", "👍 the current track"},
        {"/dislike", "👎 the current track"},
};

QString esc(QString s) {
    return s.toHtmlEscaped();
}

// Seconds → "M:SS".
QString fmtClock(int seconds) {
    if (seconds < 0) {
        seconds = 0;
    }
    return QStringLiteral("%1:%2")
            .arg(seconds / 60)
            .arg(seconds % 60, 2, 10, QLatin1Char('0'));
}

QString bubbleHtml(bool mine, const QString& who, QString content) {
    content = esc(content);
    content.replace(QStringLiteral("\n"), QStringLiteral("<br>"));
    const QString align = mine ? QStringLiteral("right") : QStringLiteral("left");
    const QString bg = mine ? QStringLiteral("#2D6FE0") : QStringLiteral("#26302B");
    const QString nameColor = mine ? QStringLiteral("#BCD8FF") : QStringLiteral("#85C85B");
    return QStringLiteral(
            "<table width='100%' cellspacing='0' cellpadding='0'><tr>"
            "<td align='%1'>"
            "<table cellspacing='0' cellpadding='8' style='max-width:560px;'>"
            "<tr><td bgcolor='%2' style='color:#F2F2F2; font-size:13px;'>"
            "<span style='color:%3; font-weight:bold; font-size:11px;'>%4</span><br>%5"
            "</td></tr></table></td></tr></table>"
            "<table cellspacing='0' cellpadding='2'><tr><td>&nbsp;</td></tr></table>")
            .arg(align, bg, nameColor, who, content);
}

QString toolLineHtml(const QJsonObject& a) {
    QString args = a.value(QStringLiteral("args")).toString();
    if (args.size() > 90) {
        args = args.left(87) + QStringLiteral("…");
    }
    return QStringLiteral(
            "<div style='color:#7FB0E8; font-size:11px; margin:1px 0 1px 6px;'>"
            "🔧 %1(%2)</div>")
            .arg(esc(a.value(QStringLiteral("tool")).toString()), esc(args));
}

// Her reasoning while she works — dim/italic so it reads as inner thought,
// distinct from the tool-call lines. (Streamed live over /ws/state.)
QString thinkLineHtml(const QJsonObject& a) {
    QString text = a.value(QStringLiteral("text")).toString();
    if (text.size() > 240) {
        text = text.left(237) + QStringLiteral("…");
    }
    return QStringLiteral(
            "<div style='color:#8A9A8A; font-size:11px; font-style:italic; "
            "margin:1px 0 1px 6px;'>💭 %1</div>")
            .arg(esc(text));
}

// Block-char energy sparkline from a list of {e:energy(0-10)} objects.
QString sparkline(const QJsonArray& arc) {
    static const QChar kBlocks[] = {
            u' ', u'▁', u'▂', u'▃', u'▄', u'▅', u'▆', u'▇', u'█'};
    QString out;
    for (const QJsonValue& v : arc) {
        double e = v.toObject().value(QStringLiteral("e")).toDouble();
        int idx = std::clamp(static_cast<int>(e / 10.0 * 8.0), 0, 8);
        QString color = e < 4 ? QStringLiteral("#2D9CDB")
                : e < 6       ? QStringLiteral("#85C85B")
                : e < 8       ? QStringLiteral("#E0A030")
                              : QStringLiteral("#E05050");
        out += QStringLiteral("<span style='color:%1;'>%2</span>")
                       .arg(color, QString(kBlocks[idx]));
    }
    return out;
}

// Which log tab(s) a line belongs to (keyword classifier, ported from the TUI).
bool inDj(const QString& t) {
    return t.contains("transition", Qt::CaseInsensitive) ||
            t.contains("deck", Qt::CaseInsensitive) ||
            t.contains("crossfad", Qt::CaseInsensitive) ||
            t.contains("auto-transition", Qt::CaseInsensitive) ||
            t.contains("emergency", Qt::CaseInsensitive);
}
bool inPlanner(const QString& t) {
    return t.contains("planner", Qt::CaseInsensitive) ||
            t.contains("playlist", Qt::CaseInsensitive) ||
            t.contains("candidate", Qt::CaseInsensitive) ||
            t.contains("knowledge", Qt::CaseInsensitive);
}
bool inLibrary(const QString& t) {
    return t.contains("download", Qt::CaseInsensitive) ||
            t.contains("enrich", Qt::CaseInsensitive) ||
            t.contains("library", Qt::CaseInsensitive) ||
            t.contains("generat", Qt::CaseInsensitive) ||
            t.contains("producer", Qt::CaseInsensitive);
}
bool inIssues(const QString& t) {
    return t.contains("error", Qt::CaseInsensitive) ||
            t.contains("warn", Qt::CaseInsensitive) ||
            t.contains("fail", Qt::CaseInsensitive) ||
            t.contains("❌") || t.contains("⚠");
}

QString logLineHtml(const QString& text) {
    const QString color = inIssues(text) ? QStringLiteral("#E08080")
                                          : QStringLiteral("#AAA");
    return QStringLiteral("<div style='color:%1; font-size:11px; margin:1px 0;'>%2</div>")
            .arg(color, esc(text));
}
} // anonymous namespace

QString DlgDJTretaChat::base() const {
    return kBase;
}

DlgDJTretaChat::DlgDJTretaChat(QWidget* parent)
        : QWidget(parent),
          m_pStatus(new QLabel(this)),
          m_pTabs(new QTabWidget(this)),
          m_pChat(new QTextBrowser(this)),
          m_pActivity(new QTextBrowser(this)),
          m_pSet(new QTextBrowser(this)),
          m_pDj(new QTextBrowser(this)),
          m_pPlanner(new QTextBrowser(this)),
          m_pLibrary(new QTextBrowser(this)),
          m_pReflect(new QTextBrowser(this)),
          m_pIssues(new QTextBrowser(this)),
          m_pInput(new QLineEdit(this)),
          m_pPollTimer(new QTimer(this)),
          m_pWs(new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this)) {
    m_pStatus->setTextFormat(Qt::RichText);
    // Word-wrap the multi-line header. Without this the long section-timeline
    // line (INTRO→BUILDUP→…) forces a huge minimum width on the whole cockpit,
    // which pins the library sidebar splitter so it can't be widened.
    m_pStatus->setWordWrap(true);
    m_pStatus->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Minimum);
    m_pStatus->setText(tr("DJ Treta — connecting…"));

    // Let the whole cockpit shrink so the sidebar splitter can give it space.
    setMinimumWidth(220);

    const auto setupBrowser = [](QTextBrowser* b) {
        b->setOpenExternalLinks(false);
        b->setFrameShape(QFrame::NoFrame);
    };
    for (QTextBrowser* b : {m_pChat, m_pActivity, m_pSet, m_pDj, m_pPlanner,
                 m_pLibrary, m_pReflect, m_pIssues}) {
        setupBrowser(b);
    }
    m_pTabs->addTab(m_pChat, tr("Chat"));
    m_pTabs->addTab(m_pActivity, tr("Activity"));
    m_pTabs->addTab(m_pSet, tr("Set"));
    m_pTabs->addTab(m_pDj, tr("DJ"));
    m_pTabs->addTab(m_pPlanner, tr("Planner"));
    m_pTabs->addTab(m_pLibrary, tr("Library"));
    m_pTabs->addTab(m_pReflect, tr("Reflect"));
    m_pTabs->addTab(m_pIssues, tr("Issues"));
    m_pTabs->tabBar()->setExpanding(false);  // tabs left-aligned, natural width

    m_pInput->setPlaceholderText(tr("Talk to DJ Treta…  (/ for commands)"));

    auto* pSend = new QPushButton(tr("Send"), this);
    auto* pSkip = new QPushButton(tr("Skip"), this);
    auto* pLike = new QPushButton(QStringLiteral("👍"), this);
    auto* pDislike = new QPushButton(QStringLiteral("👎"), this);
    auto* pDoIt = new QPushButton(tr("Do it"), this);
    auto* pNo = new QPushButton(tr("No"), this);
    for (QPushButton* b : {pSkip, pLike, pDislike, pDoIt, pNo}) {
        b->setObjectName(QStringLiteral("action"));
    }

    auto* pInputRow = new QHBoxLayout();
    pInputRow->addWidget(m_pInput, 1);
    pInputRow->addWidget(pSend);
    pInputRow->addWidget(pSkip);
    pInputRow->addWidget(pLike);
    pInputRow->addWidget(pDislike);
    pInputRow->addWidget(pDoIt);
    pInputRow->addWidget(pNo);

    auto* pLayout = new QVBoxLayout(this);
    pLayout->setContentsMargins(10, 16, 10, 10);  // extra top margin
    pLayout->setSpacing(8);
    pLayout->addWidget(m_pStatus, 0);
    pLayout->addWidget(m_pTabs, 1);
    pLayout->addLayout(pInputRow, 0);
    setLayout(pLayout);

    setStyleSheet(QStringLiteral(
            "QWidget { background: #161616; }"
            "QLabel { background: #1E1E1E; border: 1px solid #2A2A2A;"
            "  border-radius: 8px; padding: 7px 12px; color: #ECECEC; }"
            "QLabel#agents { background: transparent; border: none; padding: 0 6px; }"
            "QTextBrowser { background: #161616; border: none; }"
            "QTabWidget::pane { border: 1px solid #2A2A2A; border-radius: 6px; }"
            "QTabBar::tab { background: #1E1E1E; color: #AAA; padding: 5px 12px;"
            "  border: 1px solid #2A2A2A; border-bottom: none;"
            "  border-top-left-radius: 6px; border-top-right-radius: 6px; }"
            "QTabBar::tab:selected { background: #2D6FE0; color: white; }"
            "QLineEdit { background: #232323; color: #ECECEC; border: 1px solid #333;"
            "  border-radius: 14px; padding: 7px 12px; font-size: 13px; }"
            "QLineEdit:focus { border: 1px solid #2D6FE0; }"
            "QPushButton { background: #2D6FE0; color: white; border: none;"
            "  border-radius: 14px; padding: 7px 14px; font-weight: bold; }"
            "QPushButton#action { background: #2A2A2A; color: #DDD; padding: 7px 10px; }"
            "QPushButton:hover { background: #3a82ff; }"
            "QPushButton#action:hover { background: #383838; }"));

    connect(m_pInput, &QLineEdit::returnPressed, this, &DlgDJTretaChat::sendMessage);
    connect(pSend, &QPushButton::clicked, this, &DlgDJTretaChat::sendMessage);
    connect(pSkip, &QPushButton::clicked, this, &DlgDJTretaChat::onSkip);
    connect(pLike, &QPushButton::clicked, this, &DlgDJTretaChat::onLike);
    connect(pDislike, &QPushButton::clicked, this, &DlgDJTretaChat::onDislike);
    connect(pDoIt, &QPushButton::clicked, this, &DlgDJTretaChat::onDoIt);
    connect(pNo, &QPushButton::clicked, this, &DlgDJTretaChat::onNo);
    connect(m_pPollTimer, &QTimer::timeout, this, &DlgDJTretaChat::poll);
    connect(&m_net, &QNetworkAccessManager::finished, this, &DlgDJTretaChat::onReply);

    // Live activity feed: subscribe to the daemon's push channel so thinking
    // + tool calls stream in real time (same /ws/state the TUI uses), instead
    // of the laggy HTTP poll. Localhost needs no token.
    connect(m_pWs, &QWebSocket::connected, this, &DlgDJTretaChat::onWsConnected);
    connect(m_pWs, &QWebSocket::textMessageReceived, this, &DlgDJTretaChat::onWsTextMessage);
    connect(m_pWs, &QWebSocket::disconnected, this, &DlgDJTretaChat::onWsDisconnected);

    // Animated "…" typing indicator (. / .. / ...) while awaiting a reply.
    m_pDotTimer = new QTimer(this);
    m_pDotTimer->setInterval(380);
    connect(m_pDotTimer, &QTimer::timeout, this, [this] {
        m_dotPhase = (m_dotPhase + 1) % 3;
        m_sigChat.clear();  // force renderChat to repaint the dots
        renderChat();
    });

    setupCommandCompleter();

    // Keyboard shortcuts — scoped to the cockpit (only fire when it has focus,
    // so they don't hijack Mixxx's global hotkeys while DJing). On macOS Qt
    // maps "Ctrl" to ⌘ Cmd. Chosen to avoid text-edit conflicts.
    const auto addSc = [this](const QString& seq, void (DlgDJTretaChat::*slot)()) {
        auto* sc = new QShortcut(QKeySequence(seq), this);
        sc->setContext(Qt::WidgetWithChildrenShortcut);
        connect(sc, &QShortcut::activated, this, slot);
    };
    addSc(QStringLiteral("Ctrl+S"), &DlgDJTretaChat::onSkip);
    addSc(QStringLiteral("Ctrl+L"), &DlgDJTretaChat::onLike);
    addSc(QStringLiteral("Ctrl+G"), &DlgDJTretaChat::onDislike);
    addSc(QStringLiteral("Ctrl+Return"), &DlgDJTretaChat::onDoIt);
    addSc(QStringLiteral("Ctrl+Backspace"), &DlgDJTretaChat::onNo);
    // Cmd+1..8 → switch tabs (browser-style, no edit conflict).
    for (int i = 0; i < 8; ++i) {
        auto* sc = new QShortcut(
                QKeySequence(QStringLiteral("Ctrl+%1").arg(i + 1)), this);
        sc->setContext(Qt::WidgetWithChildrenShortcut);
        connect(sc, &QShortcut::activated, this, [this, i] {
            m_pTabs->setCurrentIndex(i);
        });
    }
}

void DlgDJTretaChat::setupCommandCompleter() {
    auto* pModel = new QStandardItemModel(this);
    for (const auto& c : kSlashCmds) {
        auto* pItem = new QStandardItem();
        pItem->setData(QStringLiteral("%1   —   %2")
                               .arg(QString::fromLatin1(c.name),
                                       QString::fromLatin1(c.desc)),
                Qt::DisplayRole);
        pItem->setData(QString(QString::fromLatin1(c.name) + QStringLiteral(" ")),
                Qt::EditRole);
        pModel->appendRow(pItem);
    }
    m_pCompleter = new QCompleter(pModel, this);
    m_pCompleter->setCompletionRole(Qt::EditRole);
    m_pCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    m_pCompleter->setFilterMode(Qt::MatchStartsWith);
    m_pCompleter->setCompletionMode(QCompleter::PopupCompletion);
    m_pInput->setCompleter(m_pCompleter);
}

void DlgDJTretaChat::onShow() {
    poll();
    m_pPollTimer->start(kPollMs);
    connectWs();
}

bool DlgDJTretaChat::hasFocus() const {
    return m_pInput->hasFocus() || QWidget::hasFocus();
}

void DlgDJTretaChat::setFocus() {
    m_pInput->setFocus();
}

void DlgDJTretaChat::sendMessage() {
    const QString text = m_pInput->text().trimmed();
    if (text.isEmpty()) {
        return;
    }
    m_pInput->clear();
    if (text.startsWith('/')) {
        handleSlashCommand(text);
        return;
    }
    m_net.get(QNetworkRequest(QUrl(base() + QStringLiteral("/http/talk?msg=") +
            QString::fromUtf8(QUrl::toPercentEncoding(text)))));
    m_pendingUserMsg = text;
    renderChat();
    QTimer::singleShot(1200, this, &DlgDJTretaChat::poll);
}

void DlgDJTretaChat::sendCommand(const QString& cmd, const QString& extraQuery) {
    // Immediate feedback in the chat so every button/shortcut/command is
    // visibly acknowledged.
    QString label = cmd;
    if (cmd == QStringLiteral("skip")) label = tr("⏭ Skipping…");
    else if (cmd == QStringLiteral("confirm_transition")) label = tr("✋→🎚 Doing it…");
    else if (cmd == QStringLiteral("reject_transition")) label = tr("✋ Dropping the suggestion…");
    else if (cmd == QStringLiteral("feedback") && extraQuery.contains(QStringLiteral("like")))
        label = tr("👍 Liked");
    else if (cmd == QStringLiteral("feedback")) label = tr("👎 Disliked");
    else if (cmd == QStringLiteral("set_mode") && extraQuery.contains(QStringLiteral("sarathi")))
        label = tr("✋ Switching to Sarathi — you drive…");
    else if (cmd == QStringLiteral("set_mode")) label = tr("🎚 Switching to Auto — she drives…");
    else if (cmd == QStringLiteral("change_mood")) label = tr("🎨 Changing the mood…");
    appendChatAction(QStringLiteral("<i>%1</i>").arg(label));

    QString url = base() + QStringLiteral("/http/command?cmd=") + cmd;
    if (!extraQuery.isEmpty()) {
        url += extraQuery;
    }
    m_net.get(QNetworkRequest(QUrl(url)));
    QTimer::singleShot(500, this, &DlgDJTretaChat::poll);
}

void DlgDJTretaChat::appendChatAction(const QString& html) {
    QJsonObject n;
    n[QStringLiteral("ts")] = QDateTime::currentMSecsSinceEpoch() / 1000.0;
    n[QStringLiteral("html")] = QStringLiteral(
            "<table width='100%'><tr><td align='center'>"
            "<span style='color:#9A9A9A; font-size:11px;'>%1</span>"
            "</td></tr></table>").arg(html);
    m_actionNotes.append(n);
    while (m_actionNotes.size() > 40) {
        m_actionNotes.removeAt(0);
    }
    m_sigChat.clear();  // force a chat repaint
    renderChat();
}

bool DlgDJTretaChat::handleSlashCommand(const QString& text) {
    const QString cmd = text.section(' ', 0, 0);
    const QString rest = text.section(' ', 1).trimmed();
    if (cmd == QStringLiteral("/auto")) {
        sendCommand(QStringLiteral("set_mode"), QStringLiteral("&mode=autonomous"));
    } else if (cmd == QStringLiteral("/sarathi")) {
        sendCommand(QStringLiteral("set_mode"), QStringLiteral("&mode=sarathi"));
    } else if (cmd == QStringLiteral("/skip")) {
        sendCommand(QStringLiteral("skip"));
    } else if (cmd == QStringLiteral("/mood")) {
        sendCommand(QStringLiteral("change_mood"), QStringLiteral("&mood=") +
                QString::fromUtf8(QUrl::toPercentEncoding(rest)));
    } else if (cmd == QStringLiteral("/accept") || cmd == QStringLiteral("/doit")) {
        sendCommand(QStringLiteral("confirm_transition"));
    } else if (cmd == QStringLiteral("/reject")) {
        sendCommand(QStringLiteral("reject_transition"));
    } else if (cmd == QStringLiteral("/like")) {
        sendCommand(QStringLiteral("feedback"), QStringLiteral("&type=like"));
    } else if (cmd == QStringLiteral("/dislike")) {
        sendCommand(QStringLiteral("feedback"), QStringLiteral("&type=dislike"));
    } else {
        appendActivityNote(QStringLiteral(
                "<div style='color:#E0A030;'>unknown command: %1</div>").arg(esc(cmd)));
        return false;
    }
    appendActivityNote(QStringLiteral(
            "<div style='color:#7FB0E8;'>» %1</div>").arg(esc(text)));
    return true;
}

void DlgDJTretaChat::onSkip() { sendCommand(QStringLiteral("skip")); }
void DlgDJTretaChat::onLike() { sendCommand(QStringLiteral("feedback"), QStringLiteral("&type=like")); }
void DlgDJTretaChat::onDislike() { sendCommand(QStringLiteral("feedback"), QStringLiteral("&type=dislike")); }
void DlgDJTretaChat::onDoIt() { sendCommand(QStringLiteral("confirm_transition")); }
void DlgDJTretaChat::onNo() { sendCommand(QStringLiteral("reject_transition")); }

void DlgDJTretaChat::appendActivityNote(const QString& html) {
    m_pActivity->append(html);
    m_pActivity->verticalScrollBar()->setValue(
            m_pActivity->verticalScrollBar()->maximum());
}

void DlgDJTretaChat::poll() {
    // NOTE: /http/activity is intentionally absent — the activity feed
    // (thinking + tool calls) now streams live over the WebSocket
    // (onWsTextMessage), so polling it would clobber the live entries.
    for (const char* path : {"/http/chat?n=40",
                 "/http/state", "/http/log?n=120", "/http/reflections",
                 "/http/tracklist", "/http/billing"}) {
        m_net.get(QNetworkRequest(QUrl(base() + QString::fromLatin1(path))));
    }
}

void DlgDJTretaChat::connectWs() {
    if (!m_pWs) {
        return;
    }
    const QAbstractSocket::SocketState st = m_pWs->state();
    if (st == QAbstractSocket::ConnectedState || st == QAbstractSocket::ConnectingState) {
        return;
    }
    m_pWs->open(QUrl(QStringLiteral("ws://127.0.0.1:7779/ws/state")));
}

void DlgDJTretaChat::onWsConnected() {
    // The server replays its thinking ring on connect, so start fresh and let
    // those replayed events rebuild the feed (avoids dupes with a stale poll).
    m_activity = QJsonArray();
}

void DlgDJTretaChat::onWsTextMessage(const QString& message) {
    const QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    if (!doc.isObject()) {
        return;
    }
    const QJsonObject o = doc.object();
    if (o.value(QStringLiteral("type")).toString() != QStringLiteral("event")) {
        return;
    }
    if (o.value(QStringLiteral("event")).toString() != QStringLiteral("thinking")) {
        return;
    }
    QJsonObject data = o.value(QStringLiteral("data")).toObject();
    if (!data.contains(QStringLiteral("ts"))) {
        data.insert(QStringLiteral("ts"),
                QDateTime::currentMSecsSinceEpoch() / 1000.0);
    }
    QJsonArray arr = m_activity;
    arr.append(data);
    while (arr.size() > 200) {  // bound the feed
        arr.removeFirst();
    }
    m_activity = arr;
    renderChat();
    renderActivity();
}

void DlgDJTretaChat::onWsDisconnected() {
    // Reconnect with a short backoff so a daemon restart self-heals.
    QTimer::singleShot(2000, this, &DlgDJTretaChat::connectWs);
}

void DlgDJTretaChat::onReply(QNetworkReply* pReply) {
    pReply->deleteLater();
    if (pReply->error() != QNetworkReply::NoError) {
        return;
    }
    const QString path = pReply->url().path();
    const QJsonDocument doc = QJsonDocument::fromJson(pReply->readAll());
    if (!doc.isObject()) {
        return;
    }
    const QJsonObject o = doc.object();
    if (path == QStringLiteral("/http/chat")) {
        const QJsonArray turns = o.value(QStringLiteral("turns")).toArray();
        if (!m_pendingUserMsg.isEmpty()) {
            for (const QJsonValue& v : turns) {
                const QJsonObject t = v.toObject();
                if (t.value(QStringLiteral("role")).toString() == QStringLiteral("user") &&
                        t.value(QStringLiteral("content")).toString() == m_pendingUserMsg) {
                    m_pendingUserMsg.clear();
                    break;
                }
            }
        }
        m_turns = turns;
        renderChat();
    } else if (path == QStringLiteral("/http/activity")) {
        m_activity = o.value(QStringLiteral("activity")).toArray();
        renderChat();
        renderActivity();
    } else if (path == QStringLiteral("/http/state")) {
        m_lastState = o;
        renderStatus(QByteArray());
        renderSet();
    } else if (path == QStringLiteral("/http/log")) {
        m_log = o.value(QStringLiteral("log")).toArray();
        renderActivity();
        renderLogs();
    } else if (path == QStringLiteral("/http/reflections")) {
        m_reflections = o.value(QStringLiteral("reflections")).toArray();
        renderReflect();
    } else if (path == QStringLiteral("/http/tracklist")) {
        m_tracklist = o.value(QStringLiteral("tracks")).toArray();
        renderSet();
    } else if (path == QStringLiteral("/http/billing")) {
        m_billing = o;
        renderStatus(QByteArray());
    } else if (path == QStringLiteral("/http/command")) {
        const QString res = o.value(QStringLiteral("result")).toString();
        if (!res.isEmpty()) {
            appendChatAction(QStringLiteral(
                    "<span style='color:#85C85B;'>✓ %1</span>").arg(esc(res)));
        }
    }
}

void DlgDJTretaChat::renderStatus(const QByteArray&) {
    const QJsonObject d = m_lastState;
    if (d.isEmpty()) {
        m_pStatus->setText(QStringLiteral(
                "<span style='color:#888;'>Brain offline — daemon not reachable</span>"));
        return;
    }
    const QJsonObject cur = d.value(QStringLiteral("current_track")).toObject();
    const QJsonObject set = d.value(QStringLiteral("set")).toObject();
    const QString phase = d.value(QStringLiteral("phase")).toString(QStringLiteral("idle"));
    const bool sarathi = d.value(QStringLiteral("sarathi_mode")).toBool();

    auto onoff = [](bool on) {
        return on ? QStringLiteral("<span style='color:#85C85B;'>ON</span>")
                  : QStringLiteral("<span style='color:#666;'>OFF</span>");
    };
    const QString sep = QStringLiteral(" <span style='color:#555;'>|</span> ");
    QString pc = QStringLiteral("#777");
    if (phase == QStringLiteral("playing")) {
        pc = QStringLiteral("#85C85B");
    } else if (phase == QStringLiteral("transitioning")) {
        pc = QStringLiteral("#2D9CDB");
    } else if (phase == QStringLiteral("preparing") || phase == QStringLiteral("starting")) {
        pc = QStringLiteral("#E0A030");
    } else if (phase == QStringLiteral("recovery")) {
        pc = QStringLiteral("#E05050");
    }

    QStringList lines;

    // ── Line 1: set info ──
    const int tracksPlayed = d.value(QStringLiteral("tracks_played")).toInt();
    const QJsonObject sources = d.value(QStringLiteral("sources")).toObject();
    QStringList srcParts;
    if (sources.value(QStringLiteral("youtube")).toBool()) {
        srcParts << QStringLiteral("YT");
    }
    if (sources.value(QStringLiteral("treta_originals")).toBool()) {
        srcParts << QStringLiteral("Originals");
    }
    const QString srcStr = srcParts.isEmpty() ? QStringLiteral("none") : srcParts.join('+');
    const QString setTitle = set.value(QStringLiteral("title")).toString();
    if (!setTitle.isEmpty()) {
        const int num = set.value(QStringLiteral("number")).toInt();
        QString genre = set.value(QStringLiteral("genre")).toString();
        if (genre.isEmpty()) {
            genre = set.value(QStringLiteral("mood")).toString();
        }
        const int elapsed = set.value(QStringLiteral("elapsed")).toInt();
        const int target = set.value(QStringLiteral("target_minutes")).toInt();
        const QString setTime = target > 0
                ? QStringLiteral("%1 / %2:00").arg(fmtClock(elapsed)).arg(target)
                : fmtClock(elapsed);
        lines << QStringLiteral(
                "<span style='color:#888;'>SET #%1</span> "
                "<b style='color:#F2F2F2;'>“%2”</b>%3"
                "<span style='color:#C792EA;'>%4</span>%5"
                "<span style='color:#AAA;'>%6</span>%7"
                "<span style='color:#AAA;'>%8 tracks</span>%9"
                "<span style='color:#C792EA;'>%10</span>")
                         .arg(QString::number(num), esc(setTitle), sep, esc(genre), sep,
                                 setTime, sep, QString::number(tracksPlayed), sep, srcStr);
    } else {
        lines << QStringLiteral(
                "<span style='color:#888;'>No set active · mood %1 · %2 tracks</span>")
                         .arg(esc(d.value(QStringLiteral("mood")).toString()),
                                 QString::number(tracksPlayed));
    }

    // ── Line 2: status bar ──
    const QString agentStr = d.value(QStringLiteral("agent_busy")).toBool()
            ? QStringLiteral("<span style='color:#E0A030;'>THINKING</span>")
            : QStringLiteral("<span style='color:#777;'>idle</span>");
    const QString plStatus = d.value(QStringLiteral("planner_status")).toString(QStringLiteral("idle"));
    const int plSince = d.value(QStringLiteral("planner_tracks_since")).toInt();
    const QString plStr = (plStatus == QStringLiteral("busy"))
            ? QStringLiteral("<span style='color:#E0A030;'>PLANNING</span>")
            : QStringLiteral("<span style='color:#777;'>idle (%1 since)</span>").arg(plSince);
    const int emerg = d.value(QStringLiteral("emergency_count")).toInt();
    const QString emergStr = emerg > 0
            ? QStringLiteral("<span style='color:#E05050;'>%1</span>").arg(emerg)
            : QStringLiteral("<span style='color:#777;'>0</span>");
    lines << QStringLiteral("<span style='color:%1;'>● %2</span>%3"
                            "Agent: %4%5Planner: %6%7Relay: %8%9REC: %10")
                          .arg(pc, phase.toUpper(), sep, agentStr, sep, plStr, sep,
                                  onoff(d.value(QStringLiteral("relay_connected")).toBool()), sep,
                                  onoff(d.value(QStringLiteral("recording")).toBool()))
            + QStringLiteral("%1BCAST: %2%3Emerg: %4")
                      .arg(sep, onoff(d.value(QStringLiteral("broadcasting")).toBool()), sep, emergStr);

    // ── Line 3: now / next ──
    QString nowNext;
    const QString curTitle = cur.value(QStringLiteral("title")).toString();
    if (!curTitle.isEmpty()) {
        nowNext = QStringLiteral("<span style='color:#888;'>Now:</span> "
                                 "<i style='color:#F2F2F2;'>%1</i>").arg(esc(curTitle));
        const int bpm = cur.value(QStringLiteral("bpm")).toInt();
        if (bpm > 0) {
            nowNext += QStringLiteral(" <span style='color:#9AC;'>%1</span>").arg(bpm);
        }
    }
    const QString nextTitle = d.value(QStringLiteral("next_track")).toObject()
                                      .value(QStringLiteral("title")).toString();
    if (!nextTitle.isEmpty()) {
        if (!nowNext.isEmpty()) {
            nowNext += QStringLiteral("　　");
        }
        nowNext += QStringLiteral("<span style='color:#888;'>Next:</span> "
                                  "<i style='color:#7FB0E8;'>%1</i>").arg(esc(nextTitle));
    }
    if (!nowNext.isEmpty()) {
        lines << nowNext;
    }

    // ── Line 4: Sarathi ──
    if (sarathi) {
        const QJsonObject sugg = d.value(QStringLiteral("pending_suggestion")).toObject();
        if (!sugg.isEmpty()) {
            const QString tech = sugg.value(QStringLiteral("technique")).toString()
                                         .toUpper().replace('_', ' ');
            const int toDeck = sugg.value(QStringLiteral("to_deck")).toInt();
            const QString sTitle = sugg.value(QStringLiteral("track_title")).toString();
            const QString reason = sugg.value(QStringLiteral("reason")).toString();
            QString l = QStringLiteral(
                    "<span style='color:#E0A030; font-weight:bold;'>✋ SARATHI</span> "
                    "she suggests <b>%1 → deck %2</b>").arg(tech).arg(toDeck);
            if (!sTitle.isEmpty()) {
                l += QStringLiteral(" <span style='color:#7FB0E8;'>(%1)</span>").arg(esc(sTitle));
            }
            if (!reason.isEmpty()) {
                l += QStringLiteral(" <span style='color:#888;'>— %1</span>").arg(esc(reason.left(70)));
            }
            lines << l;
        } else {
            lines << QStringLiteral(
                    "<span style='color:#E0A030;'>✋ SARATHI</span> "
                    "<span style='color:#777;'>you drive — no suggestion live</span>");
        }
    }

    // ── Line 5: current-track section timeline (server-formatted, optional) ──
    const QString timeline = cur.value(QStringLiteral("timeline_compact")).toString();
    if (!timeline.isEmpty()) {
        lines << QStringLiteral("<span style='color:#999; font-size:11px;'>%1</span>")
                         .arg(esc(timeline));
    }

    // ── Line 6: billing ──
    QJsonObject b = m_billing;
    if (b.isEmpty()) {
        b = d.value(QStringLiteral("billing")).toObject();
    }
    const double cost = b.value(QStringLiteral("total_cost_usd")).toDouble();
    const int calls = b.value(QStringLiteral("calls")).toInt();
    if (cost > 0 || calls > 0) {
        const qint64 tok = static_cast<qint64>(b.value(QStringLiteral("total_input_tokens")).toDouble()) +
                static_cast<qint64>(b.value(QStringLiteral("total_output_tokens")).toDouble());
        const double sessStart = b.value(QStringLiteral("session_start")).toDouble();
        double costHr = 0.0;
        if (sessStart > 0) {
            const double mins = (QDateTime::currentMSecsSinceEpoch() / 1000.0 - sessStart) / 60.0;
            if (mins > 0) {
                costHr = cost / mins * 60.0;
            }
        }
        const QString tokStr = tok > 1000000
                ? QStringLiteral("%1M").arg(tok / 1000000.0, 0, 'f', 1)
                : QStringLiteral("%1K").arg(tok / 1000);
        lines << QStringLiteral(
                "<span style='color:#666; font-size:11px;'>$%1 | %2 tokens | %3 calls | $%4/hr</span>")
                         .arg(cost, 0, 'f', 3).arg(tokStr).arg(calls).arg(costHr, 0, 'f', 3);
    }

    m_pStatus->setText(lines.join(QStringLiteral("<br>")));
}


void DlgDJTretaChat::renderChat() {
    const QString sig = QString::number(m_turns.size()) + QStringLiteral("/") +
            QString::number(m_activity.size()) + QStringLiteral("/") +
            QString::number(m_actionNotes.size()) + QStringLiteral("|") + m_pendingUserMsg;
    if (sig == m_sigChat) {
        return;
    }
    m_sigChat = sig;
    std::vector<std::pair<double, QString>> items;
    for (const QJsonValue& v : m_turns) {
        const QJsonObject t = v.toObject();
        const bool mine = t.value(QStringLiteral("role")).toString() == QStringLiteral("user");
        items.emplace_back(t.value(QStringLiteral("ts")).toDouble(),
                bubbleHtml(mine, mine ? tr("You") : tr("DJ Treta"),
                        t.value(QStringLiteral("content")).toString()));
    }
    for (const QJsonValue& v : m_actionNotes) {
        const QJsonObject n = v.toObject();
        items.emplace_back(n.value(QStringLiteral("ts")).toDouble(),
                n.value(QStringLiteral("html")).toString());
    }
    for (const QJsonValue& v : m_activity) {
        const QJsonObject a = v.toObject();
        const QString t = a.value(QStringLiteral("type")).toString();
        if (t == QStringLiteral("call")) {
            items.emplace_back(a.value(QStringLiteral("ts")).toDouble(), toolLineHtml(a));
        } else if (t == QStringLiteral("think")) {
            // Show her reasoning inline too — "full tool calls + full thinking"
            // while the user waits, streamed live over the WebSocket.
            items.emplace_back(a.value(QStringLiteral("ts")).toDouble(), thinkLineHtml(a));
        }
    }
    std::stable_sort(items.begin(), items.end(),
            [](const auto& x, const auto& y) { return x.first < y.first; });
    QString html = QStringLiteral("<html><body>");
    for (const auto& it : items) {
        html += it.second;
    }
    if (!m_pendingUserMsg.isEmpty()) {
        if (!m_pDotTimer->isActive()) {
            m_pDotTimer->start();
        }
        html += bubbleHtml(true, tr("You"), m_pendingUserMsg);
        html += bubbleHtml(false, tr("DJ Treta"), QString(m_dotPhase + 1, QChar('.')));
    } else if (m_pDotTimer->isActive()) {
        m_pDotTimer->stop();
    }
    html += QStringLiteral("</body></html>");
    m_pChat->setHtml(html);
    m_pChat->verticalScrollBar()->setValue(m_pChat->verticalScrollBar()->maximum());
}

void DlgDJTretaChat::renderActivity() {
    const QString sig = QString::number(m_activity.size()) + QStringLiteral("/") +
            QString::number(m_log.size());
    if (sig == m_sigActivity) {
        return;
    }
    m_sigActivity = sig;
    std::vector<std::pair<double, QString>> items;
    for (const QJsonValue& v : m_activity) {
        const QJsonObject a = v.toObject();
        const double ts = a.value(QStringLiteral("ts")).toDouble();
        if (a.value(QStringLiteral("type")).toString() == QStringLiteral("call")) {
            items.emplace_back(ts, toolLineHtml(a));
        } else {
            QString t = a.value(QStringLiteral("text")).toString().simplified();
            if (t.size() > 140) t = t.left(137) + QStringLiteral("…");
            items.emplace_back(ts, QStringLiteral(
                    "<div style='color:#888; font-size:11px; margin:1px 6px;'>💭 %1</div>")
                    .arg(esc(t)));
        }
    }
    for (const QJsonValue& v : m_log) {
        const QJsonObject e = v.toObject();
        items.emplace_back(e.value(QStringLiteral("ts")).toDouble(),
                logLineHtml(e.value(QStringLiteral("text")).toString()));
    }
    std::stable_sort(items.begin(), items.end(),
            [](const auto& x, const auto& y) { return x.first < y.first; });
    QString html = QStringLiteral("<html><body>");
    for (const auto& it : items) html += it.second;
    html += QStringLiteral("</body></html>");
    m_pActivity->setHtml(html);
    m_pActivity->verticalScrollBar()->setValue(m_pActivity->verticalScrollBar()->maximum());
}

void DlgDJTretaChat::renderLogs() {
    const QString sig = QString::number(m_log.size());
    if (sig == m_sigLogs) {
        return;
    }
    m_sigLogs = sig;
    QString dj = QStringLiteral("<html><body>");
    QString pl = dj, lib = dj, iss = dj;
    for (const QJsonValue& v : m_log) {
        const QString t = v.toObject().value(QStringLiteral("text")).toString();
        const QString line = logLineHtml(t);
        if (inDj(t)) dj += line;
        if (inPlanner(t)) pl += line;
        if (inLibrary(t)) lib += line;
        if (inIssues(t)) iss += line;
    }
    const QString tail = QStringLiteral("</body></html>");
    m_pDj->setHtml(dj + tail);
    m_pPlanner->setHtml(pl + tail);
    m_pLibrary->setHtml(lib + tail);
    m_pIssues->setHtml(iss + tail);
    for (QTextBrowser* b : {m_pDj, m_pPlanner, m_pLibrary, m_pIssues}) {
        b->verticalScrollBar()->setValue(b->verticalScrollBar()->maximum());
    }
}

void DlgDJTretaChat::renderSet() {
    const QJsonObject set = m_lastState.value(QStringLiteral("set")).toObject();
    const QJsonArray arc = set.value(QStringLiteral("energy_arc")).toArray();
    const QString sig = set.value(QStringLiteral("title")).toString() +
            QStringLiteral("/") + QString::number(arc.size()) +
            QStringLiteral("/") + QString::number(m_tracklist.size());
    if (sig == m_sigSet) {
        return;
    }
    m_sigSet = sig;
    QString html = QStringLiteral("<html><body style='color:#DDD; font-size:12px;'>");
    const QString title = set.value(QStringLiteral("title")).toString();
    if (!title.isEmpty()) {
        html += QStringLiteral("<p><b style='color:#F2F2F2;'>%1</b>  "
                "<span style='color:#888;'>%2</span></p>")
                        .arg(esc(title), esc(set.value(QStringLiteral("mood")).toString()));
    }
    if (!arc.isEmpty()) {
        html += QStringLiteral("<p style='font-size:15px; letter-spacing:0px;'>%1</p>")
                        .arg(sparkline(arc));
    }
    if (!m_tracklist.isEmpty()) {
        html += QStringLiteral("<p style='color:#888;'>Played:</p><ol style='color:#CCC;'>");
        for (const QJsonValue& v : m_tracklist) {
            const QJsonObject t = v.toObject();
            QString fb = t.value(QStringLiteral("feedback")).toString();
            QString icon = fb == QStringLiteral("like") ? QStringLiteral(" 👍")
                    : fb == QStringLiteral("dislike")   ? QStringLiteral(" 👎")
                                                        : QString();
            html += QStringLiteral("<li>%1%2</li>")
                            .arg(esc(t.value(QStringLiteral("title")).toString()), icon);
        }
        html += QStringLiteral("</ol>");
    } else {
        html += QStringLiteral("<p style='color:#666;'>No tracks played yet.</p>");
    }
    html += QStringLiteral("</body></html>");
    m_pSet->setHtml(html);
}

void DlgDJTretaChat::renderReflect() {
    const QString sig = QString::number(m_reflections.size());
    if (sig == m_sigReflect) {
        return;
    }
    m_sigReflect = sig;
    QString html = QStringLiteral("<html><body style='color:#CCC; font-size:12px;'>");
    if (m_reflections.isEmpty()) {
        html += QStringLiteral("<p style='color:#666;'>No reflections yet "
                "(she reflects every ~15 min).</p>");
    }
    for (int i = m_reflections.size() - 1; i >= 0; --i) {
        const QJsonObject r = m_reflections.at(i).toObject();
        const QString intent = r.value(QStringLiteral("next_intent")).toString();
        const QString drift = r.value(QStringLiteral("mood_drift_observed")).toString();
        html += QStringLiteral("<div style='border-left:2px solid #2D6FE0; "
                "padding-left:8px; margin:6px 0;'>");
        if (!intent.isEmpty()) {
            html += QStringLiteral("<b style='color:#85C85B;'>→ %1</b><br>").arg(esc(intent));
        }
        if (!drift.isEmpty()) {
            html += QStringLiteral("<span style='color:#999;'>mood: %1</span>").arg(esc(drift));
        }
        html += QStringLiteral("</div>");
    }
    html += QStringLiteral("</body></html>");
    m_pReflect->setHtml(html);
}
