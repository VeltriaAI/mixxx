#include "library/djtreta/djtretafeature.h"

#include <QDir>
#include <QFileInfoList>
#include <QStandardPaths>

#include "library/browse/foldertreemodel.h"
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
          m_pSidebarModel(new FolderTreeModel(this)) {
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

    m_musicDir = withTrailingSlash(
            QDir::homePath() + QStringLiteral("/Music/DJTreta"));

    buildSidebarTree();
}

void DJTretaFeature::buildSidebarTree() {
    std::unique_ptr<TreeItem> pRootItem = TreeItem::newRoot(this);

    // Library / Planned / Suggestions point at daemon-maintained symlink
    // folders (_all / _planned / _suggestions) so Mixxx's folder browser shows
    // them as track lists. The daemon keeps them in sync (browse_folders.py).
    pRootItem->appendChild(tr("Library"), withTrailingSlash(m_musicDir + QStringLiteral("_all")));

    // Genres — one child per real genre subfolder (skip dotfiles + the
    // synthetic _-prefixed daemon folders).
    TreeItem* pGenres = pRootItem->appendChild(tr("Genres"), m_musicDir);
    const QFileInfoList genreDirs = QDir(m_musicDir).entryInfoList(
            QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo& dir : genreDirs) {
        const QString n = dir.fileName();
        if (n.startsWith('.') || n.startsWith('_')) {
            continue;
        }
        pGenres->appendChild(n, withTrailingSlash(dir.filePath()));
    }

    pRootItem->appendChild(tr("Planned"), withTrailingSlash(m_musicDir + QStringLiteral("_planned")));
    pRootItem->appendChild(tr("Suggestions"), withTrailingSlash(m_musicDir + QStringLiteral("_suggestions")));

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
    const QString path = pItem->getData().toString();
    if (path.isEmpty()) {
        return;
    }
    auto dirInfo = mixxx::FileInfo(path);
    auto dirAccess = mixxx::FileAccess(dirInfo);
    if (!dirAccess.isReadable()) {
        if (Sandbox::askForAccess(&dirInfo)) {
            dirAccess = mixxx::FileAccess(dirInfo);
        } else {
            return;
        }
    }
    emit saveModelState();
    m_browseModel.setPath(std::move(dirAccess));
    emit showTrackModel(&m_proxyModel);
}
