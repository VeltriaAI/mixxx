#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QString>
#include <QWidget>

#include "library/libraryview.h"

class QLabel;
class QTabWidget;
class QTextBrowser;
class QLineEdit;
class QTimer;
class QNetworkReply;
class QCompleter;
class QWebSocket;

// DJ Treta cockpit — the full brain inside Mixxx, native (no terminal/web
// embed; Mixxx has neither). Layout: persistent status strip + agent row on
// top, a QTabWidget (Chat/Activity/Set/DJ/Planner/Library/Reflect/Issues) in
// the middle, input + action buttons at bottom. Mirrors the TUI 1:1. All data
// from the daemon's :7779 HTTP surface (QtNetwork polling); writes via
// /http/command. Shown when the DJ Treta sidebar root is selected.
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
    void poll();
    void onReply(QNetworkReply* pReply);
    void onSkip();
    void onLike();
    void onDislike();
    void onDoIt();
    void onNo();
    // Live activity feed over WebSocket (replaces polling /http/activity).
    void connectWs();
    void onWsConnected();
    void onWsTextMessage(const QString& message);
    void onWsDisconnected();

  private:
    void setupCommandCompleter();
    bool handleSlashCommand(const QString& text);
    void sendCommand(const QString& cmd, const QString& extraQuery = QString());
    void appendActivityNote(const QString& html);
    void appendChatAction(const QString& html);  // feedback line in the Chat tab

    void renderStatus(const QByteArray& json);
    void renderChat();
    void renderActivity();
    void renderSet();
    void renderLogs();
    void renderReflect();

    QString base() const;

    // Top strips
    QLabel* m_pStatus;
    // Tabs
    QTabWidget* m_pTabs;
    QTextBrowser* m_pChat;
    QTextBrowser* m_pActivity;
    QTextBrowser* m_pSet;
    QTextBrowser* m_pDj;
    QTextBrowser* m_pPlanner;
    QTextBrowser* m_pLibrary;
    QTextBrowser* m_pReflect;
    QTextBrowser* m_pIssues;
    // Input
    QLineEdit* m_pInput;
    QCompleter* m_pCompleter;

    QNetworkAccessManager m_net;
    QTimer* m_pPollTimer;
    QWebSocket* m_pWs;     // live push channel for thinking + tool calls
    QTimer* m_pDotTimer;   // animates the "…" typing indicator while waiting
    int m_dotPhase = 0;

    // Polled state
    QJsonArray m_turns;
    QJsonArray m_actionNotes;   // {ts, html} feedback lines for button/cmd actions
    QJsonArray m_activity;
    QJsonArray m_log;
    QJsonArray m_reflections;
    QJsonArray m_tracklist;
    QJsonObject m_lastState;
    QJsonObject m_billing;
    QString m_pendingUserMsg;
    // Dirty-check sigs (avoid flicker/scroll-jump on unchanged polls)
    QString m_sigChat, m_sigActivity, m_sigSet, m_sigLogs, m_sigReflect;
};
