#include "infrastructure/database/database.h"

#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace listenfree::infrastructure::database {

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
    return migrate();
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
    const QStringList statements{
        QStringLiteral("CREATE TABLE IF NOT EXISTS schema_migrations (version INTEGER PRIMARY KEY, checksum TEXT NOT NULL)"),
        QStringLiteral("CREATE TABLE IF NOT EXISTS tracks (track_id TEXT PRIMARY KEY, title TEXT NOT NULL, duration_ms INTEGER NOT NULL DEFAULT 0, local_path TEXT, remote_url TEXT)"),
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
        QStringLiteral("CREATE INDEX IF NOT EXISTS idx_playlist_entries_track ON playlist_entries(track_id)"),
        QStringLiteral("INSERT OR IGNORE INTO schema_migrations(version, checksum) VALUES (1, 'bootstrap-v1')")
    };
    for (const QString& statement : statements) {
        QSqlQuery query(db_);
        if (!query.exec(statement)) {
            QSqlQuery rollback(db_);
            rollback.exec(QStringLiteral("ROLLBACK"));
            return false;
        }
    }
    QSqlQuery commit(db_);
    return commit.exec(QStringLiteral("COMMIT"));
}

bool Database::upsertTrack(const domain::Track& track) {
    if (!isOpen() || track.id.empty()) return false;
    QSqlQuery query(db_);
    query.prepare(QStringLiteral("INSERT INTO tracks(track_id,title,duration_ms,local_path,remote_url) VALUES(?,?,?,?,?) ON CONFLICT(track_id) DO UPDATE SET title=excluded.title,duration_ms=excluded.duration_ms,local_path=excluded.local_path,remote_url=excluded.remote_url"));
    query.addBindValue(QString::fromStdString(track.id.value()));
    query.addBindValue(QString::fromStdString(track.title));
    query.addBindValue(track.duration.count());
    query.addBindValue(track.localPath ? QString::fromStdString(*track.localPath) : QVariant{});
    query.addBindValue(track.remoteUrl ? QString::fromStdString(*track.remoteUrl) : QVariant{});
    return query.exec();
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
        result.push_back(std::move(track));
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
    return commit.exec(QStringLiteral("COMMIT"));
}

bool Database::removePlaylist(const domain::PlaylistId& id) {
    if (!isOpen() || id.empty()) return false;
    QSqlQuery query(db_);
    query.prepare(QStringLiteral("DELETE FROM playlists WHERE playlist_id = ?"));
    query.addBindValue(QString::fromStdString(id.value()));
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
