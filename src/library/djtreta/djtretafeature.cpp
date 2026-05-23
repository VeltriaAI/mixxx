#include "library/djtreta/djtretafeature.h"

#include <QDir>
#include <QFileInfoList>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QTimer>
#include <QUrl>

#include "library/browse/foldertreemodel.h"
#include "library/djtreta/djtretatrackmodel.h"
#include "library/djtreta/dlgdjtretachat.h"
#include "library/library.h"
#include "library/trackcollectionmanager.h"
#include "library/treeitem.h"
#include "moc_djtretafeature.cpp"
#include "util/fileaccess.h"
#include "util/fileinfo.h"
#include "util/sandbox.h"
#include "widget/wlibrary.h"
#include "widget/wlibrarytextbrowser.h"

namespace {
// Clicking the DJ Treta root shows the cockpit view (chat now; grows into the
// full TUI dashboard). Child nodes (Library/Genres/...) show track tables.
const QString kViewName = QStringLiteral("DJTreta");

// Sentinel tree-item data for daemon-backed nodes (vs a real folder path).
const QString kUpNextNode = QStringLiteral("djtreta://upnext");
const QString kPlayedNode = QStringLiteral("djtreta://played");

// Daemon-node refresh cadence (her queue / set history changes live).
constexpr int kRefreshMs = 4000;

QString withTrailingSlash(QString path) {
    if (!path.endsWith('/')) {
        path.append('/');
    }
    return path;
}
} // anonymous namespace

DJTretaFeature::DJTretaFeature(
        Library* pLibrary,
        UserSettingsPointer pConfig,
        RecordingManager* pRecordingManager)
        : LibraryFeature(pLibrary, pConfig, QStringLiteral("djtreta")),
          m_pTrackCollection(pLibrary->trackCollectionManager()->internalCollection()),
          m_browseModel(this, pLibrary->trackCollectionManager(), pRecordingManager),
          m_proxyModel(&m_browseModel, true),
          m_pSidebarModel(new FolderTreeModel(this)),
          m_pTrackModel(new DJTretaTrackModel(
                  this, pLibrary->trackCollectionManager())),
          m_pRefreshTimer(new QTimer(this)) {
    connect(&m_browseModel,
            &BrowseTableModel::saveModelState,
            this,
            &LibraryFeature::saveModelState);
    connect(&m_browseModel,
            &BrowseTableModel::restoreModelState,
            this,
            &LibraryFeature::restoreModelState);

    m_proxyModel.setFilterCaseSensitivity(Qt::CaseInsensitive);
    m_proxyModel.setSortCaseSensitivity(Qt::CaseInsensitive);
    m_proxyModel.setSortRole(Qt::UserRole);
    m_proxyModel.setDynamicSortFilter(true);

    connect(&m_net,
            &QNetworkAccessManager::finished,
            this,
            &DJTretaFeature::onDaemonReply);
    m_pRefreshTimer->setInterval(kRefreshMs);
    connect(m_pRefreshTimer,
            &QTimer::timeout,
            this,
            &DJTretaFeature::refreshActiveDaemonNode);

    m_musicDir = withTrailingSlash(
            QDir::homePath() + QStringLiteral("/Music/DJTreta"));

    buildSidebarTree();
}

void DJTretaFeature::buildSidebarTree() {
    std::unique_ptr<TreeItem> pRootItem = TreeItem::newRoot(this);

    // Library node = the _all symlink folder (every track). Folder nodes feed
    // the library-backed track model (listed mp3s → resolved to library rows),
    // so they show the full columns + Overview waveform.
    pRootItem->appendChild(tr("Library"), withTrailingSlash(m_musicDir + QStringLiteral("_all")));

    // Genres — one child per real genre subfolder on disk. Skip dotfiles, the
    // synthetic _-prefixed daemon folders (_all/_planned/_suggestions), and any
    // folder that holds no audio (e.g. the "knowledge" LanceDB cache) — a genre
    // is defined by containing tracks, so this stays correct as folders change.
    static const QStringList kAudioFilters{
            QStringLiteral("*.mp3"), QStringLiteral("*.m4a"),
            QStringLiteral("*.flac"), QStringLiteral("*.wav"),
            QStringLiteral("*.aiff"), QStringLiteral("*.ogg"),
            QStringLiteral("*.opus")};
    TreeItem* pGenres = pRootItem->appendChild(tr("Genres"), m_musicDir);
    const QFileInfoList genreDirs = QDir(m_musicDir).entryInfoList(
            QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& dir : genreDirs) {
        const QString n = dir.fileName();
        if (n.startsWith('.') || n.startsWith('_')) {
            continue;
        }
        // Only list folders that actually contain audio (real genres).
        if (QDir(dir.filePath()).entryList(kAudioFilters, QDir::Files).isEmpty()) {
            continue;
        }
        pGenres->appendChild(n, withTrailingSlash(dir.filePath()));
    }

    // Daemon-backed nodes (live over :7779). Up Next = her ranked planner
    // queue (load any row to pick/override); Played = this set's history.
    pRootItem->appendChild(tr("Up Next"), kUpNextNode);
    pRootItem->appendChild(tr("Played"), kPlayedNode);

    m_pSidebarModel->setRootItem(std::move(pRootItem));
}

QVariant DJTretaFeature::title() {
    return QVariant(tr("DJ Treta"));
}

TreeItemModel* DJTretaFeature::sidebarModel() const {
    return m_pSidebarModel;
}

void DJTretaFeature::bindLibraryWidget(WLibrary* pLibraryWidget,
        KeyboardEventFilter* keyboard) {
    Q_UNUSED(keyboard);
    // The root view is the cockpit — currently the chat window (talks to the
    // daemon over :7779). This is where the rest of the TUI gets ported in.
    DlgDJTretaChat* pChat = new DlgDJTretaChat(pLibraryWidget);
    pLibraryWidget->registerView(kViewName, pChat);
}

void DJTretaFeature::activate() {
    // Root view = cockpit; no daemon table active, so stop polling.
    m_activeDaemonRoute.clear();
    m_pRefreshTimer->stop();
    emit switchToView(kViewName);
    emit enableCoverArtDisplay(false);
}

void DJTretaFeature::activateChild(const QModelIndex& index) {
    if (!index.isValid()) {
        return;
    }
    TreeItem* pItem = static_cast<TreeItem*>(index.internalPointer());
    if (!(pItem && pItem->getData().isValid())) {
        return;
    }
    const QString data = pItem->getData().toString();
    if (data.isEmpty()) {
        return;
    }

    emit saveModelState();

    // Daemon-backed node: show the (shared) track model now and pull paths
    // live; keep refreshing while this node is active.
    if (data == kUpNextNode || data == kPlayedNode) {
        m_activeDaemonRoute = (data == kUpNextNode)
                ? QStringLiteral("/http/playlist")
                : QStringLiteral("/http/tracklist");
        emit showTrackModel(m_pTrackModel);
        fetchDaemonTracks(m_activeDaemonRoute);
        m_pRefreshTimer->start();
        return;
    }

    // Folder node (Library / a genre): stop polling, list the folder's tracks
    // and show them via the library-backed model (→ waveforms).
    m_activeDaemonRoute.clear();
    m_pRefreshTimer->stop();
    m_pTrackModel->setTrackPaths(listFolderTracks(data));
    emit showTrackModel(m_pTrackModel);
}

QStringList DJTretaFeature::listFolderTracks(const QString& dir) const {
    QStringList out;
    const QStringList nameFilters{
            QStringLiteral("*.mp3"),
            QStringLiteral("*.m4a"),
            QStringLiteral("*.flac"),
            QStringLiteral("*.wav"),
            QStringLiteral("*.aiff"),
            QStringLiteral("*.ogg"),
            QStringLiteral("*.opus")};
    const QFileInfoList files = QDir(dir).entryInfoList(
            nameFilters, QDir::Files, QDir::Name);
    for (const QFileInfo& fi : files) {
        // Skip macOS AppleDouble sidecars (._foo.mp3) — 4 KB stubs.
        if (fi.fileName().startsWith(QStringLiteral("._"))) {
            continue;
        }
        // _all is a folder of symlinks → resolve to the real file so the
        // library can match it; canonicalFilePath() follows symlinks.
        const QString real = fi.canonicalFilePath();
        out << (real.isEmpty() ? fi.absoluteFilePath() : real);
    }
    return out;
}

QString DJTretaFeature::daemonBase() const {
    return QStringLiteral("http://localhost:7779");
}

void DJTretaFeature::fetchDaemonTracks(const QString& route) {
    QNetworkRequest req{QUrl(daemonBase() + route)};
    m_net.get(req);
}

void DJTretaFeature::refreshActiveDaemonNode() {
    if (!m_activeDaemonRoute.isEmpty()) {
        fetchDaemonTracks(m_activeDaemonRoute);
    }
}

void DJTretaFeature::onDaemonReply(QNetworkReply* pReply) {
    pReply->deleteLater();
    if (pReply->error() != QNetworkReply::NoError) {
        return;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(pReply->readAll());
    if (!doc.isObject()) {
        return;
    }
    // Both /http/playlist and /http/tracklist return {"tracks":[{"path":...}]}.
    QStringList paths;
    const QJsonArray tracks = doc.object().value(QStringLiteral("tracks")).toArray();
    for (const QJsonValue& v : tracks) {
        const QString p = v.toObject().value(QStringLiteral("path")).toString();
        if (!p.isEmpty()) {
            paths << p;
        }
    }
    m_pTrackModel->setTrackPaths(paths);
}
