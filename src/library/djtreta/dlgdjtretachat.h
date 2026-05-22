#pragma once

#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QString>
#include <QWidget>

#include "library/libraryview.h"

class QTextBrowser;
class QLineEdit;
class QTimer;
class QNetworkReply;

// In-Mixxx chat window for talking to DJ Treta, mirroring the TUI chat.
// Talks to the DJ Treta daemon over plain HTTP on :7779 (QtNetwork — no
// QtWebSockets dependency): sends via GET /http/talk?msg=, polls GET
// /http/chat for the conversation. Shown in the library region when the
// "Chat" node of the DJ Treta sidebar feature is selected.
class DlgDJTretaChat : public QWidget, public virtual LibraryView {
    Q_OBJECT
  public:
    explicit DlgDJTretaChat(QWidget* parent);
    ~DlgDJTretaChat() override = default;

    // LibraryView
    void onShow() override;
    bool hasFocus() const override;
    void setFocus() override;

  private slots:
    void sendMessage();
    void pollChat();
    void onReply(QNetworkReply* pReply);

  private:
    void renderTurns(const QByteArray& json);
    void rebuild();  // render m_turns (+ optimistic pending bubble) into m_pLog

    QTextBrowser* m_pLog;
    QLineEdit* m_pInput;
    QTimer* m_pPollTimer;
    QNetworkAccessManager m_net;
    QString m_base;
    QJsonArray m_turns;          // last server-confirmed turns
    QJsonArray m_activity;       // recent thinking + tool calls (visibility feed)
    QString m_pendingUserMsg;    // sent but not yet reflected by the server
    QString m_lastRenderSig;     // dirty-check to avoid flicker/scroll-jump
};
