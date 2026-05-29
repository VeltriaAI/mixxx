#pragma once

#include "library/basesqltablemodel.h"

// DJ Treta's daemon-fed track table.
//
// Unlike the folder-backed BrowseTableModel (no Overview waveform, no
// Last-Played), this is a BaseSqlTableModel over the real `library` table, so
// every standard column — including the Overview waveform — renders. The row
// set is an arbitrary list of tracks supplied by the daemon (Up-Next queue,
// played-this-set history) or by a genre/library folder listing. Tracks must
// already be in the library/TrackCollection (analyzed); paths that don't
// resolve are simply omitted.
//
// One instance is reused across the feature's nodes: setTrackPaths() rebuilds
// the backing view from the new path list and re-selects.
class DJTretaTrackModel final : public BaseSqlTableModel {
    Q_OBJECT
  public:
    DJTretaTrackModel(QObject* parent,
            TrackCollectionManager* pTrackCollectionManager);
    ~DJTretaTrackModel() override = default;

    // Replace the visible rows with the tracks at these (absolute or
    // music-dir-relative) paths, in library order. Unresolved paths drop out.
    void setTrackPaths(const QStringList& paths);

    bool isColumnInternal(int column) override;
    Capabilities getCapabilities() const override;
    void select() override;

  private:
    // Cache of the last applied path list. setTrackPaths() short-circuits
    // when the daemon's polling reply hasn't actually changed the rows —
    // without this, every 4s refresh did a full beginResetModel via
    // select(), wiping the user's selection and flickering the whole panel.
    QStringList m_lastPaths;
    bool m_pathsInitialized = false;
};
