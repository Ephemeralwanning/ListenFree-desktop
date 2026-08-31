#include "infrastructure/database/database.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include <array>

namespace listenfree::infrastructure::database {

namespace {

struct Migration {
    int version;
    QStringList statements;
};

QString migrationChecksum(const QStringList& statements) {
    return QString::fromLatin1(QCryptographicHash::hash(
        statements.join(QLatin1Char('\n')).toUtf8(), QCryptographicHash::Sha256).toHex());
}

const std::array migrations{
    Migration{1,
              {QStringLiteral("CREATE TABLE IF NOT EXISTS tracks (track_id TEXT PRIMARY KEY, title TEXT NOT NULL, duration_ms INTEGER NOT NULL DEFAULT 0, local_path TEXT, remote_url TEXT)"),
               QStringLiteral("CREATE TABLE IF NOT EXISTS artists (artist_id TEXT PRIMARY KEY, name TEXT NOT NULL)"),
               QStringLiteral("CREATE TABLE IF NOT EXISTS albums (album_id TEXT PRIMARY KEY, title TEXT NOT NULL, artwork_url TEXT)"),
               QStringLiteral("CREATE TABLE IF NOT EXISTS track_artists (track_id TEXT NOT NULL, artist_id TEXT NOT NULL, ordinal INTEGER NOT NULL, PRIMARY KEY(track_id, artist_id), FOREIGN KEY(track_id) REFERENCES tracks(track_id) ON DELETE CASCADE, FOREIGN KEY(artist_id) REFERENCES artists(artist_id) ON DELETE CASCADE)"),
               QStringLiteral("CREATE TABLE IF NOT EXISTS playlists (playlist_id TEXT PRIMARY KEY, title TEXT NOT NULL)"),
               QStringLiteral("CREATE TABLE IF NOT EXISTS playlist_entries (entry_id TEXT PRIMARY KEY, playlist_id TEXT NOT NULL, track_id TEXT NOT NULL, position INTEGER NOT NULL, FOREIGN KEY(playlist_id) REFERENCES playlists(playlist_id) ON DELETE CASCADE, FOREIGN KEY(track_id) REFERENCES tracks(track_id) ON DELETE CASCADE)"),
               QStringLiteral("CREATE TABLE IF NOT EXISTS local_files (canonical_path TEXT PRIMARY KEY, track_id TEXT NOT NULL, size_bytes INTEGER NOT NULL DEFAULT 0, modified_ms INTEGER NOT NULL DEFAULT 0, FOREIGN KEY(track_id) REFERENCES tracks(track_id) ON DELETE CASCADE)"),
               QStringLiteral("CREATE TABLE IF NOT EXISTS play_history (id INTEGER PRIMARY KEY AUTOINCREMENT, track_id TEXT NOT NULL, played_at_ms INTEGER NOT NULL, FOREIGN KEY(track_id) REFERENCES tracks(track_id) ON DELETE CASCADE)"),
               QStringLiteral("CREATE TABLE IF NOT EXISTS settings (key TEXT PRIMARY KEY, value TEXT NOT NULL, value_type TEXT NOT NULL)"),
               QStringLiteral("CREATE TABLE IF NOT EXISTS provider_cache (provider_id TEXT NOT NULL, cache_key TEXT NOT NULL, payload TEXT NOT NULL, expires_at_ms INTEGER NOT NULL, PRIMARY KEY(provider_id, cache_key))"),
               QStringLiteral("CREATE INDEX IF NOT EXISTS idx_tracks_title ON tracks(title)"),
               QStringLiteral("CREATE INDEX IF NOT EXISTS idx_playlist_entries_track ON playlist_entries(track_id)")}},
    Migration{2,
              {QStringLiteral("CREATE TABLE IF NOT EXISTS track_albums (track_id TEXT PRIMARY KEY, album_id TEXT NOT NULL, FOREIGN KEY(track_id) REFERENCES tracks(track_id) ON DELETE CASCADE, FOREIGN KEY(album_id) REFERENCES albums(album_id) ON DELETE CASCADE)"),
               QStringLiteral("CREATE INDEX IF NOT EXISTS idx_track_albums_album ON track_albums(album_id)"),
               QStringLiteral("CREATE INDEX IF NOT EXISTS idx_local_files_track ON local_files(track_id)")}},
};

void rollback(QSqlDatabase& database) {
    QSqlQuery query(database);
    query.exec(QStringLiteral("ROLLBACK"));
}

void hydrateRelations(const QSqlDatabase& database, domain::Track& track) {
    QSqlQuery artists(database);
    artists.prepare(QStringLiteral(
        "SELECT a.artist_id,a.name FROM track_artists ta JOIN artists a ON a.artist_id=ta.artist_id "
        "WHERE ta.track_id=? ORDER BY ta.ordinal"));
    artists.addBindValue(QString::fromStdString(track.id.value()));
    if (artists.exec()) {
        while (artists.next()) {
            track.artists.push_back({artists.value(0).toString().toStdString(),
                                     artists.value(1).toString().toStdString()});
        }
    }
    QSqlQuery album(database);
    album.prepare(QStringLiteral(
        "SELECT a.album_id,a.title,a.artwork_url FROM track_albums ta "
        "JOIN albums a ON a.album_id=ta.album_id WHERE ta.track_id=?"));
    album.addBindValue(QString::fromStdString(track.id.value()));
    if (album.exec() && album.next()) {
        domain::Album value;
        value.id = album.value(0).toString().toStdString();
        value.title = album.value(1).toString().toStdString();
        if (!album.value(2).isNull()) value.artworkUrl = album.value(2).toString().toStdString();
        track.album = std::move(value);
    }
}

} // namespace

Database::~Database() { close(); }

bool Database::open(const QString& path) {
    close();
    connectionName_ = QStringLiteral("listenfree_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName_);
    db_.setDatabaseName(path);
    if (!db_.open()) {
        close();
        return false;
    }
    if (!migrate()) {
        close();
        return false;
    }
    return true;
}

void Database::close() noexcept {
    if (connectionName_.isEmpty()) return;
    if (db_.isValid()) db_.close();
    db_ = {};
    QSqlDatabase::removeDatabase(connectionName_);
    connectionName_.clear();
}

bool Database::migrate() {
    if (!isOpen()) return false;
    QSqlQuery pragma(db_);
    if (!pragma.exec(QStringLiteral("PRAGMA foreign_keys = ON"))) return false;
    QSqlQuery transaction(db_);
    if (!transaction.exec(QStringLiteral("BEGIN IMMEDIATE"))) return false;
    {
        QSqlQuery query(db_);
        if (!query.exec(QStringLiteral(
                "CREATE TABLE IF NOT EXISTS schema_migrations (version INTEGER PRIMARY KEY, checksum TEXT NOT NULL)"))) {
            QSqlQuery rollback(db_);
            rollback.exec(QStringLiteral("ROLLBACK"));
            return false;
        }
    }
    for (const auto& migration : migrations) {
        const QString expectedChecksum = migrationChecksum(migration.statements);
        QSqlQuery existing(db_);
        existing.prepare(QStringLiteral("SELECT checksum FROM schema_migrations WHERE version = ?"));
        existing.addBindValue(migration.version);
        if (!existing.exec()) {
            rollback(db_);
            return false;
        }
        const bool applied = existing.next();
        if (applied) {
            const QString storedChecksum = existing.value(0).toString();
            const bool legacyBootstrap = migration.version == 1 &&
                                         storedChecksum == QStringLiteral("bootstrap-v1");
            if (storedChecksum != expectedChecksum && !legacyBootstrap) {
                rollback(db_);
                return false;
            }
            if (legacyBootstrap) {
                QSqlQuery upgrade(db_);
                upgrade.prepare(QStringLiteral(
                    "UPDATE schema_migrations SET checksum = ? WHERE version = ?"));
                upgrade.addBindValue(expectedChecksum);
                upgrade.addBindValue(migration.version);
                if (!upgrade.exec()) {
                    rollback(db_);
                    return false;
                }
            }
        }
        for (const QString& statement : migration.statements) {
            QSqlQuery query(db_);
            if (!query.exec(statement)) {
                rollback(db_);
                return false;
            }
        }
        if (!applied) {
            QSqlQuery insert(db_);
            insert.prepare(QStringLiteral(
                "INSERT INTO schema_migrations(version, checksum) VALUES (?, ?)"));
            insert.addBindValue(migration.version);
            insert.addBindValue(expectedChecksum);
            if (!insert.exec()) {
                rollback(db_);
                return false;
            }
        }
    }
    QSqlQuery commit(db_);
    if (!commit.exec(QStringLiteral("COMMIT"))) {
        rollback(db_);
        return false;
    }
    return true;
}

bool Database::upsertTrack(const domain::Track& track) {
    const std::array tracks{track};
    return upsertTracks(tracks);
}

bool Database::upsertTracks(std::span<const domain::Track> tracks) {
    if (!isOpen()) return false;
    QSqlQuery begin(db_);
    if (!begin.exec(QStringLiteral("BEGIN IMMEDIATE"))) return false;
    for (const auto& track : tracks) {
        if (!upsertTrackRows(track)) {
            rollback(db_);
            return false;
        }
    }
    QSqlQuery commit(db_);
    if (!commit.exec(QStringLiteral("COMMIT"))) {
        rollback(db_);
        return false;
    }
    return true;
}

bool Database::upsertTrackRows(const domain::Track& track) {
    if (track.id.empty()) return false;
    const QString trackId = QString::fromStdString(track.id.value());
    QSqlQuery query(db_);
    query.prepare(QStringLiteral("INSERT INTO tracks(track_id,title,duration_ms,local_path,remote_url) VALUES(?,?,?,?,?) ON CONFLICT(track_id) DO UPDATE SET title=excluded.title,duration_ms=excluded.duration_ms,local_path=excluded.local_path,remote_url=excluded.remote_url"));
    query.addBindValue(trackId);
    query.addBindValue(QString::fromStdString(track.title));
    query.addBindValue(track.duration.count());
    query.addBindValue(track.localPath ? QString::fromStdString(*track.localPath) : QVariant{});
    query.addBindValue(track.remoteUrl ? QString::fromStdString(*track.remoteUrl) : QVariant{});
    if (!query.exec()) {
        rollback(db_);
        return false;
    }
    for (const QString& table : {QStringLiteral("track_artists"), QStringLiteral("track_albums"),
                                 QStringLiteral("local_files")}) {
        QSqlQuery clear(db_);
        clear.prepare(QStringLiteral("DELETE FROM %1 WHERE track_id = ?").arg(table));
        clear.addBindValue(trackId);
        if (!clear.exec()) {
            rollback(db_);
            return false;
        }
    }
    int ordinal = 0;
    for (const auto& artist : track.artists) {
        if (artist.id.empty() || artist.name.empty()) continue;
        QSqlQuery upsertArtist(db_);
        upsertArtist.prepare(QStringLiteral(
            "INSERT INTO artists(artist_id,name) VALUES(?,?) "
            "ON CONFLICT(artist_id) DO UPDATE SET name=excluded.name"));
        upsertArtist.addBindValue(QString::fromStdString(artist.id));
        upsertArtist.addBindValue(QString::fromStdString(artist.name));
        if (!upsertArtist.exec()) {
            rollback(db_);
            return false;
        }
        QSqlQuery link(db_);
        link.prepare(QStringLiteral(
            "INSERT INTO track_artists(track_id,artist_id,ordinal) VALUES(?,?,?)"));
        link.addBindValue(trackId);
        link.addBindValue(QString::fromStdString(artist.id));
        link.addBindValue(ordinal++);
        if (!link.exec()) {
            rollback(db_);
            return false;
        }
    }
    if (track.album && !track.album->id.empty() && !track.album->title.empty()) {
        QSqlQuery upsertAlbum(db_);
        upsertAlbum.prepare(QStringLiteral(
            "INSERT INTO albums(album_id,title,artwork_url) VALUES(?,?,?) "
            "ON CONFLICT(album_id) DO UPDATE SET title=excluded.title,artwork_url=excluded.artwork_url"));
        upsertAlbum.addBindValue(QString::fromStdString(track.album->id));
        upsertAlbum.addBindValue(QString::fromStdString(track.album->title));
        upsertAlbum.addBindValue(track.album->artworkUrl
                                     ? QVariant(QString::fromStdString(*track.album->artworkUrl))
                                     : QVariant{});
        if (!upsertAlbum.exec()) {
            rollback(db_);
            return false;
        }
        QSqlQuery link(db_);
        link.prepare(QStringLiteral("INSERT INTO track_albums(track_id,album_id) VALUES(?,?)"));
        link.addBindValue(trackId);
        link.addBindValue(QString::fromStdString(track.album->id));
        if (!link.exec()) {
            rollback(db_);
            return false;
        }
    }
    if (track.localPath) {
        const QFileInfo file(QString::fromStdString(*track.localPath));
        const QString canonicalPath = file.canonicalFilePath();
        if (!canonicalPath.isEmpty() && file.isFile()) {
            QSqlQuery localFile(db_);
            localFile.prepare(QStringLiteral(
                "INSERT INTO local_files(canonical_path,track_id,size_bytes,modified_ms) "
                "VALUES(?,?,?,?) ON CONFLICT(canonical_path) DO UPDATE SET "
                "track_id=excluded.track_id,size_bytes=excluded.size_bytes,modified_ms=excluded.modified_ms"));
            localFile.addBindValue(canonicalPath);
            localFile.addBindValue(trackId);
            localFile.addBindValue(file.size());
            localFile.addBindValue(file.lastModified().toMSecsSinceEpoch());
            if (!localFile.exec()) {
                rollback(db_);
                return false;
            }
        }
    }
    return true;
}

std::vector<domain::Track> Database::loadTracks() const {
    std::vector<domain::Track> result;
    if (!isOpen()) return result;
    QSqlQuery query(db_);
    if (!query.exec(QStringLiteral("SELECT track_id,title,duration_ms,local_path,remote_url FROM tracks ORDER BY title"))) return result;
    while (query.next()) {
        domain::Track track;
        track.id = domain::TrackId(query.value(0).toString().toStdString());
        track.title = query.value(1).toString().toStdString();
        track.duration = std::chrono::milliseconds(query.value(2).toLongLong());
        if (!query.value(3).isNull()) track.localPath = query.value(3).toString().toStdString();
        if (!query.value(4).isNull()) track.remoteUrl = query.value(4).toString().toStdString();
        hydrateRelations(db_, track);
        result.push_back(std::move(track));
    }
    return result;
}

std::vector<application::LocalFileFingerprint> Database::loadLocalFiles() const {
    std::vector<application::LocalFileFingerprint> result;
    if (!isOpen()) return result;
    QSqlQuery query(db_);
    if (!query.exec(QStringLiteral(
            "SELECT canonical_path,size_bytes,modified_ms FROM local_files ORDER BY canonical_path"))) {
        return result;
    }
    while (query.next()) {
        result.push_back({std::filesystem::path(query.value(0).toString().toStdWString()),
                          static_cast<std::uintmax_t>(query.value(1).toULongLong()),
                          query.value(2).toLongLong()});
    }
    return result;
}

std::vector<domain::Playlist> Database::loadPlaylists() const {
    std::vector<domain::Playlist> result;
    if (!isOpen()) return result;
    QSqlQuery playlists(db_);
    if (!playlists.exec(QStringLiteral("SELECT playlist_id,title FROM playlists ORDER BY title"))) return result;
    while (playlists.next()) {
        domain::Playlist playlist;
        playlist.id = domain::PlaylistId(playlists.value(0).toString().toStdString());
        playlist.title = playlists.value(1).toString().toStdString();
        QSqlQuery entries(db_);
        entries.prepare(QStringLiteral("SELECT entry_id,track_id,position FROM playlist_entries WHERE playlist_id = ? ORDER BY position"));
        entries.addBindValue(playlists.value(0));
        if (!entries.exec()) return {};
        while (entries.next()) {
            playlist.entries.push_back({entries.value(0).toString().toStdString(),
                                        domain::TrackId(entries.value(1).toString().toStdString()),
                                        entries.value(2).toInt()});
        }
        result.push_back(std::move(playlist));
    }
    return result;
}

bool Database::savePlaylist(const domain::Playlist& playlist) {
    if (!isOpen() || playlist.id.empty() || playlist.title.empty()) return false;
    QSqlQuery begin(db_);
    if (!begin.exec(QStringLiteral("BEGIN IMMEDIATE"))) return false;
    QSqlQuery upsert(db_);
    upsert.prepare(QStringLiteral("INSERT INTO playlists(playlist_id,title) VALUES(?,?) ON CONFLICT(playlist_id) DO UPDATE SET title=excluded.title"));
    upsert.addBindValue(QString::fromStdString(playlist.id.value()));
    upsert.addBindValue(QString::fromStdString(playlist.title));
    if (!upsert.exec()) {
        QSqlQuery rollback(db_);
        rollback.exec(QStringLiteral("ROLLBACK"));
        return false;
    }
    QSqlQuery clear(db_);
    clear.prepare(QStringLiteral("DELETE FROM playlist_entries WHERE playlist_id = ?"));
    clear.addBindValue(QString::fromStdString(playlist.id.value()));
    if (!clear.exec()) {
        QSqlQuery rollback(db_);
        rollback.exec(QStringLiteral("ROLLBACK"));
        return false;
    }
    for (const auto& entry : playlist.entries) {
        if (entry.entryId.empty() || entry.trackId.empty()) continue;
        QSqlQuery insert(db_);
        insert.prepare(QStringLiteral("INSERT INTO playlist_entries(entry_id,playlist_id,track_id,position) VALUES(?,?,?,?)"));
        insert.addBindValue(QString::fromStdString(entry.entryId));
        insert.addBindValue(QString::fromStdString(playlist.id.value()));
        insert.addBindValue(QString::fromStdString(entry.trackId.value()));
        insert.addBindValue(entry.position);
        if (!insert.exec()) {
            QSqlQuery rollback(db_);
            rollback.exec(QStringLiteral("ROLLBACK"));
            return false;
        }
    }
    QSqlQuery commit(db_);
    if (!commit.exec(QStringLiteral("COMMIT"))) {
        rollback(db_);
        return false;
    }
    return true;
}

bool Database::removePlaylist(const domain::PlaylistId& id) {
    if (!isOpen() || id.empty()) return false;
    QSqlQuery query(db_);
    query.prepare(QStringLiteral("DELETE FROM playlists WHERE playlist_id = ?"));
    query.addBindValue(QString::fromStdString(id.value()));
    return query.exec();
}

bool Database::recordPlayHistory(const domain::TrackId& id,
                                 std::chrono::system_clock::time_point when) {
    if (!isOpen() || id.empty()) return false;
    const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(
        when.time_since_epoch()).count();
    QSqlQuery query(db_);
    query.prepare(QStringLiteral(
        "INSERT INTO play_history(track_id,played_at_ms) VALUES(?,?)"));
    query.addBindValue(QString::fromStdString(id.value()));
    query.addBindValue(milliseconds);
    return query.exec();
}

std::optional<domain::Track> Database::findTrack(const domain::TrackId& id) const {
    if (!isOpen() || id.empty()) return std::nullopt;
    QSqlQuery query(db_);
    query.prepare(QStringLiteral("SELECT track_id,title,duration_ms,local_path,remote_url FROM tracks WHERE track_id = ?"));
    query.addBindValue(QString::fromStdString(id.value()));
    if (!query.exec() || !query.next()) return std::nullopt;
    domain::Track track;
    track.id = domain::TrackId(query.value(0).toString().toStdString());
    track.title = query.value(1).toString().toStdString();
    track.duration = std::chrono::milliseconds(query.value(2).toLongLong());
    if (!query.value(3).isNull()) track.localPath = query.value(3).toString().toStdString();
    if (!query.value(4).isNull()) track.remoteUrl = query.value(4).toString().toStdString();
    hydrateRelations(db_, track);
    return track;
}

std::vector<domain::Track> Database::searchTracks(const QString& queryText) const {
    std::vector<domain::Track> result;
    if (!isOpen()) return result;
    QSqlQuery query(db_);
    query.prepare(QStringLiteral("SELECT track_id,title,duration_ms,local_path,remote_url FROM tracks WHERE title LIKE ? ORDER BY title"));
    query.addBindValue(QStringLiteral("%").append(queryText).append(QStringLiteral("%")));
    if (!query.exec()) return result;
    while (query.next()) {
        domain::Track track;
        track.id = domain::TrackId(query.value(0).toString().toStdString());
        track.title = query.value(1).toString().toStdString();
        track.duration = std::chrono::milliseconds(query.value(2).toLongLong());
        if (!query.value(3).isNull()) track.localPath = query.value(3).toString().toStdString();
        if (!query.value(4).isNull()) track.remoteUrl = query.value(4).toString().toStdString();
        hydrateRelations(db_, track);
        result.push_back(std::move(track));
    }
    return result;
}

std::optional<std::string> Database::getSetting(const QString& key) const {
    if (!isOpen() || key.isEmpty()) return std::nullopt;
    QSqlQuery query(db_);
    query.prepare(QStringLiteral("SELECT value FROM settings WHERE key = ?"));
    query.addBindValue(key);
    if (!query.exec() || !query.next()) return std::nullopt;
    return query.value(0).toString().toStdString();
}

bool Database::setSetting(const QString& key, const QString& value, const QString& valueType) {
    if (!isOpen() || key.isEmpty() || valueType.isEmpty()) return false;
    QSqlQuery query(db_);
    query.prepare(QStringLiteral("INSERT INTO settings(key,value,value_type) VALUES(?,?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value,value_type=excluded.value_type"));
    query.addBindValue(key);
    query.addBindValue(value);
    query.addBindValue(valueType);
    return query.exec();
}

} // namespace listenfree::infrastructure::database
