#include "library/djtreta/djtretatrackmodel.h"

#include <QSqlQuery>

#include "library/dao/trackschema.h"
#include "library/queryutil.h"
#include "library/trackcollection.h"
#include "library/trackcollectionmanager.h"
#include "mixer/playerinfo.h"
#include "moc_djtretatrackmodel.cpp"
#include "track/trackid.h"

namespace {
const QString kTableName = QStringLiteral("djtreta_view");
} // anonymous namespace

DJTretaTrackModel::DJTretaTrackModel(QObject* parent,
        TrackCollectionManager* pTrackCollectionManager)
        : BaseSqlTableModel(parent, pTrackCollectionManager, "mixxx.db.model.djtreta") {
    // Start empty; the feature calls setTrackPaths() on node activation.
    setTrackPaths(QStringList());
}

void DJTretaTrackModel::setTrackPaths(const QStringList& paths) {
    // Resolve the supplied file paths to library track ids. Only tracks
    // already in the library resolve — that's intentional, it's what gives us
    // the analyzed columns + waveform.
    const QList<TrackId> trackIds =
            m_pTrackCollectionManager->resolveTrackIdsFromLocations(paths);

    QStringList idList;
    idList.reserve(trackIds.size());
    for (const TrackId& id : trackIds) {
        idList << id.toString();
    }
    // Empty set → a WHERE that matches nothing (valid, shows zero rows).
    const QString idFilter = idList.isEmpty()
            ? QStringLiteral("0")
            : QStringLiteral("library.id IN (%1)").arg(idList.join(","));

    QStringList columns;
    columns << "library." + LIBRARYTABLE_ID
            << "'' AS " + LIBRARYTABLE_PREVIEW
            << LIBRARYTABLE_COVERART_DIGEST + " AS " + LIBRARYTABLE_COVERART;

    QSqlQuery query(m_database);
    query.prepare(QStringLiteral("DROP VIEW IF EXISTS ") + kTableName);
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
    }
    query.prepare(
            "CREATE TEMPORARY VIEW " + kTableName +
            " AS SELECT " + columns.join(",") +
            " FROM library "
            "INNER JOIN track_locations "
            "ON library.location=track_locations.id "
            "WHERE (mixxx_deleted=0 AND fs_deleted=0) AND " + idFilter);
    if (!query.exec()) {
        LOG_FAILED_QUERY(query);
    }

    QStringList tableColumns;
    tableColumns << LIBRARYTABLE_ID;
    tableColumns << LIBRARYTABLE_PREVIEW;
    tableColumns << LIBRARYTABLE_COVERART;
    setTable(kTableName,
            LIBRARYTABLE_ID,
            std::move(tableColumns),
            m_pTrackCollectionManager->internalCollection()->getTrackSource());
    setSearch("");
    setDefaultSort(fieldIndex("artist"), Qt::AscendingOrder);
    select();
}

bool DJTretaTrackModel::isColumnInternal(int column) {
    return column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_ID) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_URL) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_CUEPOINT) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_SAMPLERATE) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_MIXXXDELETED) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_HEADERPARSED) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_KEY_ID) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_BPM_LOCK) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_BEATS_VERSION) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_CHANNELS) ||
            column == fieldIndex(ColumnCache::COLUMN_TRACKLOCATIONSTABLE_DIRECTORY) ||
            column == fieldIndex(ColumnCache::COLUMN_TRACKLOCATIONSTABLE_FSDELETED) ||
            (PlayerInfo::instance().numPreviewDecks() == 0 &&
                    column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_PREVIEW)) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COVERART_SOURCE) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COVERART_TYPE) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COVERART_LOCATION) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COVERART_COLOR) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COVERART_DIGEST) ||
            column == fieldIndex(ColumnCache::COLUMN_LIBRARYTABLE_COVERART_HASH);
}

TrackModel::Capabilities DJTretaTrackModel::getCapabilities() const {
    // Read-only-ish: load to deck/sampler/preview + sort + properties.
    // No editing/hiding/removal — these tables mirror the daemon's state.
    return Capability::LoadToDeck |
            Capability::LoadToSampler |
            Capability::LoadToPreviewDeck |
            Capability::Properties |
            Capability::Sorting;
}

void DJTretaTrackModel::select() {
    BaseSqlTableModel::select();
}
