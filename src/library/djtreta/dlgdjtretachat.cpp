#include "library/djtreta/dlgdjtretachat.h"

#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QTextBrowser>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "moc_dlgdjtretachat.cpp"

namespace {
const QString kBase = QStringLiteral("http://127.0.0.1:7779");
constexpr int kPollMs = 1500;
} // anonymous namespace

DlgDJTretaChat::DlgDJTretaChat(QWidget* parent)
        : QWidget(parent),
          m_pLog(new QTextBrowser(this)),
          m_pInput(new QLineEdit(this)),
          m_pPollTimer(new QTimer(this)),
          m_base(kBase),
          m_lastTurnCount(-1) {
    m_pLog->setOpenExternalLinks(false);
    m_pInput->setPlaceholderText(tr("Talk to DJ Treta…  (e.g. \"energy badhao\")"));

    auto* pSend = new QPushButton(tr("Send"), this);

    auto* pInputRow = new QHBoxLayout();
    pInputRow->addWidget(m_pInput, 1);
    pInputRow->addWidget(pSend, 0);

    auto* pLayout = new QVBoxLayout(this);
    pLayout->setContentsMargins(8, 8, 8, 8);
    pLayout->addWidget(m_pLog, 1);
    pLayout->addLayout(pInputRow, 0);
    setLayout(pLayout);

    connect(m_pInput, &QLineEdit::returnPressed, this, &DlgDJTretaChat::sendMessage);
    connect(pSend, &QPushButton::clicked, this, &DlgDJTretaChat::sendMessage);
    connect(m_pPollTimer, &QTimer::timeout, this, &DlgDJTretaChat::pollChat);
    connect(&m_net, &QNetworkAccessManager::finished, this, &DlgDJTretaChat::onReply);
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
    const QString url = m_base + QStringLiteral("/http/talk?msg=") +
            QString::fromUtf8(QUrl::toPercentEncoding(text));
    m_net.get(QNetworkRequest(QUrl(url)));
    // Show the user's line immediately; the reply arrives on a later poll.
    m_pLog->append(QStringLiteral("<b>You:</b> %1").arg(text.toHtmlEscaped()));
    // Poll a touch sooner so the reply shows up quickly.
    QTimer::singleShot(800, this, &DlgDJTretaChat::pollChat);
}

void DlgDJTretaChat::pollChat() {
    m_net.get(QNetworkRequest(QUrl(m_base + QStringLiteral("/http/chat?n=40"))));
}

void DlgDJTretaChat::onReply(QNetworkReply* pReply) {
    pReply->deleteLater();
    if (pReply->error() != QNetworkReply::NoError) {
        return;
    }
    if (pReply->url().path() == QStringLiteral("/http/chat")) {
        renderTurns(pReply->readAll());
    }
}

void DlgDJTretaChat::renderTurns(const QByteArray& json) {
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) {
        return;
    }
    const QJsonArray turns = doc.object().value(QStringLiteral("turns")).toArray();
    // Only re-render when the turn count changed, to avoid clobbering scroll
    // and flicker on every poll.
    if (turns.size() == m_lastTurnCount) {
        return;
    }
    m_lastTurnCount = turns.size();

    m_pLog->clear();
    for (const QJsonValue& v : turns) {
        const QJsonObject t = v.toObject();
        const QString role = t.value(QStringLiteral("role")).toString();
        const QString content = t.value(QStringLiteral("content")).toString();
        const QString who = (role == QStringLiteral("user"))
                ? QStringLiteral("You")
                : QStringLiteral("DJ Treta");
        m_pLog->append(QStringLiteral("<b>%1:</b> %2")
                               .arg(who, content.toHtmlEscaped()));
    }
}
