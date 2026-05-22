#pragma once

#include <QVariant>

#include "library/browse/browsetablemodel.h"
#include "library/libraryfeature.h"
#include "library/proxytrackmodel.h"
#include "preferences/usersettings.h"

class Library;
class TrackCollection;
class RecordingManager;
class FolderTreeModel;
class WLibrary;
class KeyboardEventFilter;

// DJ Treta — the AI co-founder's library surface in the sidebar.
//
// Sits alongside Serato/Rekordbox as a first-class LibraryFeature. Scoped to
// the DJ Treta music dir (~/Music/DJTreta); child nodes map to the library
// root + each genre subfolder, so selecting a node shows its tracks in the
// standard track table — browsable + loadable via the FLX4's existing nav.
// Reuses BrowseTableModel (folder-backed), so no custom track model here.
//
// Daemon-driven nodes (Planned queue, Sarathi Suggestions over :7779) land in
// a later phase with their own model; this phase delivers the sidebar item +
// genre browsing.
class DJTretaFeature : public LibraryFeature {
    Q_OBJECT
  public:
    DJTretaFeature(Library* pLibrary,
            UserSettingsPointer pConfig,
            RecordingManager* pRecordingManager);
    ~DJTretaFeature() override = default;

    QVariant title() override;

    void bindLibraryWidget(WLibrary* libraryWidget,
            KeyboardEventFilter* keyboard) override;

    TreeItemModel* sidebarModel() const override;

  public slots:
    void activate() override;
    void activateChild(const QModelIndex& index) override;

  private:
    void buildSidebarTree();

    TrackCollection* const m_pTrackCollection;
    BrowseTableModel m_browseModel;
    ProxyTrackModel m_proxyModel;
    FolderTreeModel* m_pSidebarModel;
    QString m_musicDir;
};
