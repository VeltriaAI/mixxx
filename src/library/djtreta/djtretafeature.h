#pragma once

#include <QNetworkAccessManager>
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
class DJTretaTrackModel;
class QTimer;
class QNetworkReply;

// DJ Treta — the AI co-founder's library surface in the sidebar.
//
// Sits alongside Serato/Rekordbox as a first-class LibraryFeature. Scoped to
// the DJ Treta music dir (~/Music/DJTreta); child nodes map to the library
// root + each genre subfolder, so selecting a node shows its tracks in the
// standard track table — browsable + loadable via the FLX4's existing nav.
// Child nodes use DJTretaTrackModel (library-backed), so the full column set
// — incl. the Overview waveform — renders. Folder nodes (Library/Genres) list
// their mp3s; daemon nodes (Up Next / Played, over :7779) pull paths live and
// refresh on a timer.
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

  private slots:
    void onDaemonReply(QNetworkReply* pReply);
    void refreshActiveDaemonNode();

  private:
    void buildSidebarTree();
    // List a folder's audio files (resolving symlinks) as absolute paths.
    QStringList listFolderTracks(const QString& dir) const;
    // Async GET base()+route; reply slot pushes paths into m_pTrackModel.
    void fetchDaemonTracks(const QString& route);
    QString daemonBase() const;

    TrackCollection* const m_pTrackCollection;
    BrowseTableModel m_browseModel;
    ProxyTrackModel m_proxyModel;
    FolderTreeModel* m_pSidebarModel;
    QString m_musicDir;

    // Library-backed table shared across all child nodes.
    DJTretaTrackModel* m_pTrackModel;
    // Daemon polling for the Up-Next / Played nodes.
    QNetworkAccessManager m_net;
    QTimer* m_pRefreshTimer;
    QString m_activeDaemonRoute;  // "" when a folder node is shown
};
