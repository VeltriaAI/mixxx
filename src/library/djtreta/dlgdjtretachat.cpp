#include "library/djtreta/dlgdjtretachat.h"

#include <algorithm>
#include <utility>
#include <vector>

#include <QAbstractItemView>
#include <QCompleter>
#include <QDateTime>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QScrollBar>
#include <QTextBrowser>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "moc_dlgdjtretachat.cpp"

namespace {
const QString kBase = QStringLiteral("http://127.0.0.1:7779");
constexpr int kPollMs = 1500;

// Slash commands offered in the chat (Claude-Code-style popup on "/").
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
};
} // anonymous namespace

DlgDJTretaChat::DlgDJTretaChat(QWidget* parent)
        : QWidget(parent),
          m_pStatus(new QLabel(this)),
          m_pAgents(new QLabel(this)),
          m_pLog(new QTextBrowser(this)),
          m_pInput(new QLineEdit(this)),
          m_pPollTimer(new QTimer(this)),
          m_base(kBase) {
    m_pStatus->setTextFormat(Qt::RichText);
    m_pStatus->setText(tr("DJ Treta — connecting…"));
    m_pAgents->setTextFormat(Qt::RichText);
    m_pAgents->setObjectName(QStringLiteral("agents"));
    m_pLog->setOpenExternalLinks(false);
    m_pLog->setFrameShape(QFrame::NoFrame);
    m_pInput->setPlaceholderText(tr("Talk to DJ Treta…  (/ for commands)"));

    auto* pSend = new QPushButton(tr("Send"), this);

    // Dark, themed chrome to match the LateNight skin (the bubbles themselves
    // are themed in renderTurns).
    setStyleSheet(QStringLiteral(
            "QWidget { background: #161616; }"
            "QLabel { background: #1E1E1E; border: 1px solid #2A2A2A;"
            "  border-radius: 8px; padding: 7px 12px; color: #ECECEC; }"
            "QLabel#agents { background: transparent; border: none;"
            "  padding: 0 6px; }"
            "QTextBrowser { background: #161616; border: none; }"
            "QLineEdit {"
            "  background: #232323; color: #ECECEC; border: 1px solid #333;"
            "  border-radius: 14px; padding: 7px 12px; font-size: 13px; }"
            "QLineEdit:focus { border: 1px solid #2D6FE0; }"
            "QPushButton {"
            "  background: #2D6FE0; color: white; border: none;"
            "  border-radius: 14px; padding: 7px 18px; font-weight: bold; }"
            "QPushButton:hover { background: #3a82ff; }"
            "QPushButton:pressed { background: #1f57b8; }"));

    auto* pInputRow = new QHBoxLayout();
    pInputRow->addWidget(m_pInput, 1);
    pInputRow->addWidget(pSend, 0);

    auto* pLayout = new QVBoxLayout(this);
    pLayout->setContentsMargins(8, 8, 8, 8);
    pLayout->setSpacing(6);
    pLayout->addWidget(m_pStatus, 0);
    pLayout->addWidget(m_pAgents, 0);
    pLayout->addWidget(m_pLog, 1);
    pLayout->addLayout(pInputRow, 0);
    setLayout(pLayout);

    connect(m_pInput, &QLineEdit::returnPressed, this, &DlgDJTretaChat::sendMessage);
    connect(pSend, &QPushButton::clicked, this, &DlgDJTretaChat::sendMessage);
    connect(m_pPollTimer, &QTimer::timeout, this, &DlgDJTretaChat::pollChat);
    connect(&m_net, &QNetworkAccessManager::finished, this, &DlgDJTretaChat::onReply);

    setupCommandCompleter();
}

void DlgDJTretaChat::setupCommandCompleter() {
    // Popup of slash commands shown as you type "/". The popup DISPLAYS
    // "/cmd — description" but the completion that gets inserted is just
    // "/cmd " (EditRole) — like Claude Code's slash menu.
    auto* pModel = new QStandardItemModel(this);
    for (const auto& c : kSlashCmds) {
        auto* pItem = new QStandardItem();
        pItem->setData(QStringLiteral("%1   —   %2")
                               .arg(QString::fromLatin1(c.name),
                                       QString::fromLatin1(c.desc)),
                Qt::DisplayRole);
        pItem->setData(QString(QString::fromLatin1(c.name) + QStringLiteral(" ")), Qt::EditRole);
        pModel->appendRow(pItem);
    }
    m_pCompleter = new QCompleter(pModel, this);
    m_pCompleter->setCompletionRole(Qt::EditRole);  // match + insert "/cmd "
    m_pCompleter->setCaseSensitivity(Qt::CaseInsensitive);
    m_pCompleter->setFilterMode(Qt::MatchStartsWith);
    m_pCompleter->setCompletionMode(QCompleter::PopupCompletion);
    m_pInput->setCompleter(m_pCompleter);
}

void DlgDJTretaChat::onShow() {
    pollChat();
    m_pPollTimer->start(kPollMs);
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
    const QString url = m_base + QStringLiteral("/http/talk?msg=") +
            QString::fromUtf8(QUrl::toPercentEncoding(text));
    m_net.get(QNetworkRequest(QUrl(url)));
    // Optimistic: show the message + a "typing…" bubble now. The daemon only
    // writes the turn to the JSONL once Treta has replied, so we'd otherwise
    // stare at nothing for several seconds.
    m_pendingUserMsg = text;
    rebuild();
    QTimer::singleShot(1200, this, &DlgDJTretaChat::pollChat);
}

bool DlgDJTretaChat::handleSlashCommand(const QString& text) {
    const QString cmd = text.section(' ', 0, 0);
    const QString rest = text.section(' ', 1).trimmed();
    QString url;
    if (cmd == QStringLiteral("/auto")) {
        url = m_base + QStringLiteral("/http/command?cmd=set_mode&mode=autonomous");
    } else if (cmd == QStringLiteral("/sarathi")) {
        url = m_base + QStringLiteral("/http/command?cmd=set_mode&mode=sarathi");
    } else if (cmd == QStringLiteral("/skip")) {
        url = m_base + QStringLiteral("/http/command?cmd=skip");
    } else if (cmd == QStringLiteral("/mood")) {
        url = m_base + QStringLiteral("/http/command?cmd=change_mood&mood=") +
                QString::fromUtf8(QUrl::toPercentEncoding(rest));
    } else if (cmd == QStringLiteral("/accept") || cmd == QStringLiteral("/doit")) {
        url = m_base + QStringLiteral("/http/command?cmd=confirm_transition");
    } else if (cmd == QStringLiteral("/reject")) {
        url = m_base + QStringLiteral("/http/command?cmd=reject_transition");
    } else {
        return false;
    }
    m_net.get(QNetworkRequest(QUrl(url)));
    // State/activity will reflect the effect on the next poll.
    QTimer::singleShot(600, this, &DlgDJTretaChat::pollChat);
    return true;
}

void DlgDJTretaChat::pollChat() {
    m_net.get(QNetworkRequest(QUrl(m_base + QStringLiteral("/http/chat?n=40"))));
    m_net.get(QNetworkRequest(QUrl(m_base + QStringLiteral("/http/activity?n=60"))));
    m_net.get(QNetworkRequest(QUrl(m_base + QStringLiteral("/http/state"))));
}

void DlgDJTretaChat::onReply(QNetworkReply* pReply) {
    pReply->deleteLater();
    if (pReply->error() != QNetworkReply::NoError) {
        return;
    }
    const QString path = pReply->url().path();
    if (path == QStringLiteral("/http/chat")) {
        renderTurns(pReply->readAll());
    } else if (path == QStringLiteral("/http/activity")) {
        const QJsonDocument doc = QJsonDocument::fromJson(pReply->readAll());
        if (doc.isObject()) {
            m_activity = doc.object().value(QStringLiteral("activity")).toArray();
            rebuild();
            renderAgents();
        }
    } else if (path == QStringLiteral("/http/state")) {
        renderStatus(pReply->readAll());
    }
}

void DlgDJTretaChat::renderAgents() {
    struct Ag {
        const char* key;   // matched against the event 'agent' field
        const char* icon;
        const char* label;
    };
    static const Ag kAgents[] = {
            {"treta", "🧠", "treta"},
            {"dj_treta", "🎧", "dj"},
            {"planner", "📋", "planner"},
            {"consciousness", "💭", "mind"},
            {"mixer", "🎛", "mixer"},
    };
    const double now = QDateTime::currentMSecsSinceEpoch() / 1000.0;

    // Latest activity entry per agent (by substring match on the agent field).
    QString out;
    for (const auto& ag : kAgents) {
        QString status = QStringLiteral("idle");
        QString color = QStringLiteral("#666");
        const QString key = QString::fromLatin1(ag.key);
        // Walk newest→oldest; first match within 30s wins.
        for (int i = m_activity.size() - 1; i >= 0; --i) {
            const QJsonObject e = m_activity.at(i).toObject();
            if (!e.value(QStringLiteral("agent")).toString().contains(key)) {
                continue;
            }
            if (now - e.value(QStringLiteral("ts")).toDouble() > 30.0) {
                break;  // most recent for this agent is stale → idle
            }
            if (e.value(QStringLiteral("type")).toString() == QStringLiteral("call")) {
                status = QStringLiteral("🔧 %1").arg(
                        e.value(QStringLiteral("tool")).toString());
                color = QStringLiteral("#7FB0E8");
            } else {
                status = QStringLiteral("thinking");
                color = QStringLiteral("#E0A030");
            }
            break;
        }
        // Live overrides from state flags.
        if (key == QStringLiteral("dj_treta") &&
                m_lastState.value(QStringLiteral("agent_busy")).toBool()) {
            status = QStringLiteral("thinking");
            color = QStringLiteral("#E0A030");
        }
        if (key == QStringLiteral("planner") &&
                m_lastState.value(QStringLiteral("planner_status")).toString() ==
                        QStringLiteral("busy")) {
            status = QStringLiteral("planning");
            color = QStringLiteral("#E0A030");
        }
        out += QStringLiteral(
                "<span style='color:%1; font-size:11px;'>%2 %3</span>"
                "<span style='color:#444;'>  ·  </span>")
                       .arg(color,
                               QString::fromUtf8(ag.icon),
                               status == QStringLiteral("idle")
                                       ? QString::fromUtf8(ag.label)
                                       : status);
    }
    m_pAgents->setText(out);
}

void DlgDJTretaChat::renderStatus(const QByteArray& json) {
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) {
        return;
    }
    const QJsonObject d = doc.object();
    m_lastState = d;
    renderAgents();
    const QJsonObject cur = d.value(QStringLiteral("current_track")).toObject();
    const QJsonObject set = d.value(QStringLiteral("set")).toObject();

    const bool playing = d.value(QStringLiteral("phase")).toString() ==
            QStringLiteral("playing");
    const QString dot = playing ? QStringLiteral("#85C85B") : QStringLiteral("#777");
    const bool sarathi = d.value(QStringLiteral("sarathi_mode")).toBool();
    const QString modeStr = sarathi ? tr("SARATHI") : tr("AUTO");
    const QString modeColor = sarathi ? QStringLiteral("#E0A030") : QStringLiteral("#2D9CDB");

    QString nowLine;
    const QString title = cur.value(QStringLiteral("title")).toString();
    if (!title.isEmpty()) {
        const int bpm = cur.value(QStringLiteral("bpm")).toInt();
        nowLine = QStringLiteral("<b style='color:#F2F2F2;'>%1</b>").arg(title.toHtmlEscaped());
        if (bpm > 0) {
            nowLine += QStringLiteral("  <span style='color:#9AC;'>%1 BPM</span>").arg(bpm);
        }
    } else {
        nowLine = QStringLiteral("<span style='color:#888;'>nothing playing</span>");
    }

    QString setLine;
    const QString setTitle = set.value(QStringLiteral("title")).toString();
    if (!setTitle.isEmpty()) {
        const int elapsed = set.value(QStringLiteral("elapsed")).toInt() / 60;
        const int target = set.value(QStringLiteral("target_minutes")).toInt();
        const QString mood = set.value(QStringLiteral("mood")).toString();
        setLine = QStringLiteral(
                "<span style='color:#999; font-size:11px;'>“%1” · %2 · %3/%4 min</span>")
                          .arg(setTitle.toHtmlEscaped(), mood.toHtmlEscaped())
                          .arg(elapsed)
                          .arg(target);
    }

    m_pStatus->setText(QStringLiteral(
            "<span style='color:%1;'>●</span> "
            "<span style='color:%2; font-weight:bold; font-size:11px;'>%3</span>　%4<br>%5")
            .arg(dot, modeColor, modeStr, nowLine, setLine));
}

namespace {
QString bubbleHtml(bool mine, const QString& who, QString content) {
    content = content.toHtmlEscaped();
    content.replace(QStringLiteral("\n"), QStringLiteral("<br>"));
    const QString align = mine ? QStringLiteral("right") : QStringLiteral("left");
    const QString bg = mine ? QStringLiteral("#2D6FE0")    // accent blue
                            : QStringLiteral("#26302B");   // dark green-grey
    const QString nameColor = mine ? QStringLiteral("#BCD8FF")
                                   : QStringLiteral("#85C85B");
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

// Compact, dim, left-aligned line for a thinking step or tool call — "in
// short" visibility into what Treta is doing, interleaved with the bubbles.
QString activityHtml(const QJsonObject& a) {
    const QString type = a.value(QStringLiteral("type")).toString();
    QString body;
    QString color;
    if (type == QStringLiteral("call")) {
        QString args = a.value(QStringLiteral("args")).toString();
        if (args.size() > 70) {
            args = args.left(67) + QStringLiteral("…");
        }
        body = QStringLiteral("🔧 %1(%2)")
                       .arg(a.value(QStringLiteral("tool")).toString().toHtmlEscaped(),
                               args.toHtmlEscaped());
        color = QStringLiteral("#7FB0E8");  // tool = soft blue
    } else { // think
        QString text = a.value(QStringLiteral("text")).toString().simplified();
        if (text.size() > 130) {
            text = text.left(127) + QStringLiteral("…");
        }
        body = QStringLiteral("💭 %1").arg(text.toHtmlEscaped());
        color = QStringLiteral("#777777");  // thinking = grey
    }
    return QStringLiteral(
            "<div style='color:%1; font-size:11px; margin:1px 0 1px 6px;'>%2</div>")
            .arg(color, body);
}
} // anonymous namespace

void DlgDJTretaChat::renderTurns(const QByteArray& json) {
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) {
        return;
    }
    m_turns = doc.object().value(QStringLiteral("turns")).toArray();

    // If the server now reflects our pending message (the daemon writes the
    // user+assistant turn together once Treta replies), drop the optimism.
    if (!m_pendingUserMsg.isEmpty()) {
        for (const QJsonValue& v : m_turns) {
            const QJsonObject t = v.toObject();
            if (t.value(QStringLiteral("role")).toString() == QStringLiteral("user") &&
                    t.value(QStringLiteral("content")).toString() == m_pendingUserMsg) {
                m_pendingUserMsg.clear();
                break;
            }
        }
    }
    rebuild();
}

void DlgDJTretaChat::rebuild() {
    // Dirty-check so periodic polls don't reset scroll / flicker when nothing
    // changed.
    const QString sig = QString::number(m_turns.size()) + QStringLiteral("/") +
            QString::number(m_activity.size()) + QStringLiteral("|") +
            m_pendingUserMsg;
    if (sig == m_lastRenderSig) {
        return;
    }
    m_lastRenderSig = sig;

    // Merge chat turns (bubbles) + activity (compact lines) into one timeline,
    // ordered by timestamp, so you see: your message → her thinking → tool
    // calls → her reply.
    std::vector<std::pair<double, QString>> items;
    for (const QJsonValue& v : m_turns) {
        const QJsonObject t = v.toObject();
        const bool mine = t.value(QStringLiteral("role")).toString() ==
                QStringLiteral("user");
        items.emplace_back(t.value(QStringLiteral("ts")).toDouble(),
                bubbleHtml(mine, mine ? tr("You") : tr("DJ Treta"),
                        t.value(QStringLiteral("content")).toString()));
    }
    for (const QJsonValue& v : m_activity) {
        const QJsonObject a = v.toObject();
        // Skip "think" entries: for the chat agent the thinking text IS the
        // reply (Gemini emits no separate reasoning), so it just duplicates
        // the bubble. Tool calls are the real, non-duplicate visibility.
        if (a.value(QStringLiteral("type")).toString() != QStringLiteral("call")) {
            continue;
        }
        items.emplace_back(a.value(QStringLiteral("ts")).toDouble(), activityHtml(a));
    }
    std::stable_sort(items.begin(), items.end(),
            [](const auto& x, const auto& y) { return x.first < y.first; });

    QString html = QStringLiteral(
            "<html><body style='font-family:\"Open Sans\",sans-serif;'>");
    for (const auto& it : items) {
        html += it.second;
    }
    if (!m_pendingUserMsg.isEmpty()) {
        html += bubbleHtml(true, tr("You"), m_pendingUserMsg);
        html += bubbleHtml(false, tr("DJ Treta"), QStringLiteral("…"));
    }
    html += QStringLiteral("</body></html>");

    m_pLog->setHtml(html);
    m_pLog->verticalScrollBar()->setValue(m_pLog->verticalScrollBar()->maximum());
}
