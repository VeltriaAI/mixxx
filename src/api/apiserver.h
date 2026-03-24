#pragma once

#include <QObject>
#include <QThread>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <memory>

class PlayerManager;

/// HTTP API Server for DJ Treta
/// Full control over Mixxx — decks, mixer, library, effects, sync.
/// Everything the DJ software can do, the API can do.
class ApiServer : public QObject {
    Q_OBJECT

  public:
    explicit ApiServer(int port, PlayerManager* pPlayerManager, QObject* parent = nullptr);
    ~ApiServer();

    void start();
    void stop();

  private:
    void run();

    // Helpers
    double getControl(const QString& group, const QString& key);
    void setControl(const QString& group, const QString& key, double value);
    QString deckGroup(int deck);
    QJsonObject getTrackInfo(int deck);

    int m_port;
    PlayerManager* m_pPlayerManager;
    std::unique_ptr<QThread> m_thread;
    bool m_running = false;
};
