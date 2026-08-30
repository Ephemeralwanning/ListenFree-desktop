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

} // namespace listenfree::infrastructure::database
