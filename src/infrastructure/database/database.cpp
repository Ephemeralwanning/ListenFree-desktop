#include "infrastructure/database/database.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

#include <algorithm>
#include <array>
#include <unordered_map>

namespace listenfree::infrastructure::database {

namespace {

// Mirrors fooyin's per-connection pragma setup (src/utils/database/dbconnectionpool.cpp):
// WAL keeps the GUI reader connection concurrent with the scan writer thread's
// connection, and busy_timeout absorbs residual lock windows instead of failing
// commits. Journal/synchronous changes are harmless no-ops on :memory:.
void applyConnectionPragmas(QSqlDatabase& database) {
    QSqlQuery foreignKeys(database);
    foreignKeys.exec(QStringLiteral("PRAGMA foreign_keys = ON"));
    QSqlQuery journal(database);
    journal.exec(QStringLiteral("PRAGMA journal_mode = WAL"));
    QSqlQuery synchronous(database);
    synchronous.exec(QStringLiteral("PRAGMA synchronous = NORMAL"));
    QSqlQuery busyTimeout(database);
    busyTimeout.exec(QStringLiteral("PRAGMA busy_timeout = 5000"));
}

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
    // Music library roots, mirroring Strawberry's directories table
    // (collectionbackend.cpp AddDirectory): one row per root, path lookup
    // deduplicates, rowid ordering preserves insertion order.
    Migration{3,
              {QStringLiteral("CREATE TABLE IF NOT EXISTS library_folders (folder_id INTEGER PRIMARY KEY AUTOINCREMENT, path TEXT NOT NULL UNIQUE COLLATE NOCASE, added_ms INTEGER NOT NULL DEFAULT 0)")}},
    Migration{4, {QStringLiteral("CREATE TABLE IF NOT EXISTS duplicate_aliases (path TEXT PRIMARY KEY, keeper_id TEXT NOT NULL REFERENCES tracks(track_id) ON DELETE CASCADE, keeper_path TEXT NOT NULL, hash TEXT NOT NULL, size_bytes INTEGER NOT NULL, modified_ms INTEGER NOT NULL)")}},
    Migration{5, {QStringLiteral("CREATE TABLE IF NOT EXISTS library_exclusions (path TEXT PRIMARY KEY COLLATE NOCASE)")}},
};

void rollback(QSqlDatabase& database) {
    QSqlQuery query(database);
    query.exec(QStringLiteral("ROLLBACK"));
}

QString normalizedLibraryFolder(const QString& path) {
    if (path.isEmpty()) return {};
    const QFileInfo info(path);
    if (info.exists()) {
        if (!info.isDir()) return {};
        return QDir(info.absoluteFilePath()).canonicalPath();
    }
    return QDir(QDir::cleanPath(info.absoluteFilePath())).absolutePath();
}

QString libraryPrefixPattern(const QString& path) {
    QString escaped = path;
    escaped.replace(QStringLiteral("\\"), QStringLiteral("\\\\"))
        .replace(QStringLiteral("%"), QStringLiteral("\\%"))
        .replace(QStringLiteral("_"), QStringLiteral("\\_"));
    return escaped + (escaped.endsWith('/') ? QStringLiteral("%") : QStringLiteral("/%"));
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
bool Database::clearScrollPositions() {QSqlQuery query(db_);return query.exec("DELETE FROM settings WHERE key LIKE 'scroll.%'");}

bool Database::mergeDuplicate(const QVariantMap& duplicate,const QVariantMap& keeper,const QString& hash,bool alias) {
    if (!db_.transaction()) return false;
    const auto oldId=duplicate.value("id").toString(), newId=keeper.value("id").toString();
    const auto oldPath=duplicate.value("path").toString(), newPath=keeper.value("path").toString();
    auto execute=[&](const QString& sql,const QVariantList& args) {
        QSqlQuery query(db_); query.prepare(sql); for(const auto& arg:args)query.addBindValue(arg); return query.exec();
    };
    QSqlQuery check(db_); check.prepare("SELECT COUNT(*) FROM tracks WHERE (track_id=? AND replace(local_path,'\\','/')=? COLLATE NOCASE) OR (track_id=? AND replace(local_path,'\\','/')=? COLLATE NOCASE)");
    for(const auto& arg:{oldId,oldPath,newId,newPath}) check.addBindValue(arg);
    if (oldId==newId || !check.exec() || !check.next() || check.value(0).toInt()!=2) {db_.rollback();return false;}
    bool ok=execute("UPDATE playlist_entries SET track_id=? WHERE track_id=?",{newId,oldId}) &&
            execute("UPDATE play_history SET track_id=? WHERE track_id=?",{newId,oldId});
    // The portable collection/queue snapshots contain mixed local and online identities.
    const auto redirect=[&](auto&& self,QJsonValue value)->QJsonValue {
        if(value.isArray()){QJsonArray result;for(const auto& child:value.toArray())result.append(self(self,child));return result;}
        if(!value.isObject())return value;
        auto object=value.toObject();
        if(QDir::fromNativeSeparators(object.value("localPath").toString()).compare(oldPath,Qt::CaseInsensitive)==0){
            object["localPath"]=newPath;object["trackId"]=newId;
            object.remove("artwork");
        }
        for(auto it=object.begin();it!=object.end();++it)if(it.value().isObject()||it.value().isArray())it.value()=self(self,it.value());
        return object;
    };
    for(const auto& key:{QString("collections.v1"),QString("portable.queue")}) {
        const auto raw=getSetting(key); if(!raw)continue;
        const auto document=QJsonDocument::fromJson(QByteArray::fromStdString(*raw));
        const auto result=redirect(redirect,document.isArray()?QJsonValue(document.array()):QJsonValue(document.object()));
        ok=ok && setSetting(key,QString::fromUtf8((result.isArray()?QJsonDocument(result.toArray()):QJsonDocument(result.toObject())).toJson(QJsonDocument::Compact)));
    }
    if(alias)ok=ok && execute("INSERT OR REPLACE INTO duplicate_aliases(path,keeper_id,keeper_path,hash,size_bytes,modified_ms) VALUES(?,?,?,?,?,?)",
        {oldPath,newId,newPath,hash,duplicate.value("size"),duplicate.value("modified")});
    ok=ok && execute("UPDATE duplicate_aliases SET keeper_id=?,keeper_path=? WHERE keeper_id=?",{newId,newPath,oldId});
    ok=ok && execute("DELETE FROM tracks WHERE track_id=?",{oldId});
    if(ok && db_.commit())return true;
    db_.rollback();return false;
}

bool Database::applySettings(const QVariantMap& values, const QStringList& addedRoots, const std::function<bool()>& applyRuntime) {
    if (!db_.transaction()) return false;
    for (auto it=values.begin();it!=values.end();++it) {
        if (!setSetting(it.key(),it.value().toString())) { db_.rollback(); return false; }
    }
    for (const auto& root:addedRoots) {
        if (!addLibraryFolder(root)) { db_.rollback(); return false; }
    }
    if (applyRuntime && !applyRuntime()) {db_.rollback();return false;}
    if (db_.commit()) return true;
    db_.rollback(); return false;
}

bool Database::connect(const QString& path) {
    close();
    connectionName_ = QStringLiteral("listenfree_%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    db_ = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName_);
    db_.setDatabaseName(path);
    if (!db_.open()) {
        close();
        return false;
    }
    applyConnectionPragmas(db_);
    return true;
}

bool Database::open(const QString& path) {
    if (!connect(path)) return false;
    if (!migrate()) {
        close();
        return false;
    }
    return true;
}

bool Database::openExisting(const QString& path) { return connect(path); }

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
            existing.finish();
            const bool legacyBootstrap = migration.version == 1 &&
                                         storedChecksum == QStringLiteral("bootstrap-v1");
            // A portable build shipped v4 before its keeper foreign key was
            // added. Accept only that exact historical checksum, and upgrade
            // the actual table before recording the current checksum.
            const bool legacyAliases = migration.version == 4 && storedChecksum ==
                QStringLiteral("9730c9c84b3dd5e4c8d9783d056aa8483f9f287bc0abc8f8167b5f83a3198412");
            if (storedChecksum != expectedChecksum && !legacyBootstrap && !legacyAliases) {
                rollback(db_);
                return false;
            }
            if (legacyAliases) {
                const QStringList upgradeStatements{
                    QStringLiteral("ALTER TABLE duplicate_aliases RENAME TO duplicate_aliases_legacy_v4"),
                    migration.statements.front(),
                    // Aliases whose keeper was already removed are stale scan
                    // hints. Removing those hints lets the file be rediscovered.
                    QStringLiteral("INSERT INTO duplicate_aliases(path,keeper_id,keeper_path,hash,size_bytes,modified_ms) SELECT a.path,a.keeper_id,a.keeper_path,a.hash,a.size_bytes,a.modified_ms FROM duplicate_aliases_legacy_v4 a JOIN tracks t ON t.track_id=a.keeper_id"),
                    QStringLiteral("DROP TABLE duplicate_aliases_legacy_v4")
                };
                for (const auto& statement : upgradeStatements) {
                    QSqlQuery upgrade(db_);
                    if (!upgrade.exec(statement)) { rollback(db_); return false; }
                }
            }
            if (legacyBootstrap || legacyAliases) {
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

UpsertStatements::UpsertStatements(QSqlDatabase& database)
    : track(database), clearArtists(database), clearAlbums(database), clearFiles(database),
      artist(database), linkArtist(database), album(database), linkAlbum(database), localFile(database) {
    track.prepare(QStringLiteral("INSERT INTO tracks(track_id,title,duration_ms,local_path,remote_url) VALUES(?,?,?,?,?) ON CONFLICT(track_id) DO UPDATE SET title=excluded.title,duration_ms=excluded.duration_ms,local_path=excluded.local_path,remote_url=excluded.remote_url"));
    clearArtists.prepare(QStringLiteral("DELETE FROM track_artists WHERE track_id = ?"));
    clearAlbums.prepare(QStringLiteral("DELETE FROM track_albums WHERE track_id = ?"));
    clearFiles.prepare(QStringLiteral("DELETE FROM local_files WHERE track_id = ?"));
    artist.prepare(QStringLiteral(
        "INSERT INTO artists(artist_id,name) VALUES(?,?) "
        "ON CONFLICT(artist_id) DO UPDATE SET name=excluded.name"));
    linkArtist.prepare(QStringLiteral("INSERT INTO track_artists(track_id,artist_id,ordinal) VALUES(?,?,?)"));
    album.prepare(QStringLiteral(
        "INSERT INTO albums(album_id,title,artwork_url) VALUES(?,?,?) "
        "ON CONFLICT(album_id) DO UPDATE SET title=excluded.title,artwork_url=excluded.artwork_url"));
    linkAlbum.prepare(QStringLiteral("INSERT INTO track_albums(track_id,album_id) VALUES(?,?)"));
    localFile.prepare(QStringLiteral(
            "INSERT INTO local_files(canonical_path,track_id,size_bytes,modified_ms) "
            "VALUES(?,?,?,?) ON CONFLICT(canonical_path) DO UPDATE SET "
            "track_id=excluded.track_id,size_bytes=excluded.size_bytes,modified_ms=excluded.modified_ms"));
}

bool Database::upsertTracks(std::span<const domain::Track> tracks, bool fromScan) {
    if (!isOpen()) return false;
    QSqlQuery begin(db_);
    if (!begin.exec(QStringLiteral("BEGIN IMMEDIATE"))) return false;
    UpsertStatements statements(db_);
    QSqlQuery excluded(db_);
    if(fromScan && !excluded.prepare("SELECT 1 FROM library_exclusions WHERE path=?")) { rollback(db_); return false; }
    for (const auto& track : tracks) {
        if(fromScan && track.localPath) {
            const auto path=QDir::cleanPath(QDir::fromNativeSeparators(QString::fromStdString(*track.localPath)));
            // The file can be trashed after metadata was read but before this
            // batch reaches the writer, including during a cancelled scan.
            if(!QFileInfo(path).isFile())continue;
            excluded.bindValue(0, path);
            if(!excluded.exec()) { rollback(db_); return false; }
            const bool skip = excluded.next(); excluded.finish();
            // Check under the same write transaction, including batches that
            // were read before the user removed this file from the library.
            if(skip)continue;
        }
        if (!upsertTrackRows(track, statements)) {
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
    UpsertStatements statements(db_);
    return upsertTrackRows(track, statements);
}

bool Database::upsertTrackRows(const domain::Track& track, UpsertStatements& statements) {
    if (track.id.empty()) return false;
    const QString trackId = QString::fromStdString(track.id.value());
    if(track.localPath) {
        QSqlQuery clearAlias(db_); clearAlias.prepare("DELETE FROM duplicate_aliases WHERE path=?");
        clearAlias.addBindValue(QDir::fromNativeSeparators(QString::fromStdString(*track.localPath)));
        if(!clearAlias.exec())return false;
    }
    QSqlQuery& query = statements.track;
    query.addBindValue(trackId);
    query.addBindValue(QString::fromStdString(track.title));
    query.addBindValue(track.duration.count());
    query.addBindValue(track.localPath ? QString::fromStdString(*track.localPath) : QVariant{});
    query.addBindValue(track.remoteUrl ? QString::fromStdString(*track.remoteUrl) : QVariant{});
    if (!query.exec()) {
        rollback(db_);
        return false;
    }
    query.finish();
    for (QSqlQuery* clear : {&statements.clearArtists, &statements.clearAlbums, &statements.clearFiles}) {
        clear->addBindValue(trackId);
        if (!clear->exec()) {
            rollback(db_);
            return false;
        }
        clear->finish();
    }
    int ordinal = 0;
    for (const auto& artist : track.artists) {
        if (artist.id.empty() || artist.name.empty()) continue;
        QSqlQuery& upsertArtist = statements.artist;
        upsertArtist.addBindValue(QString::fromStdString(artist.id));
        upsertArtist.addBindValue(QString::fromStdString(artist.name));
        if (!upsertArtist.exec()) {
            rollback(db_);
            return false;
        }
        upsertArtist.finish();
        QSqlQuery& link = statements.linkArtist;
        link.addBindValue(trackId);
        link.addBindValue(QString::fromStdString(artist.id));
        link.addBindValue(ordinal++);
        if (!link.exec()) {
            rollback(db_);
            return false;
        }
        link.finish();
    }
    if (track.album && !track.album->id.empty() && !track.album->title.empty()) {
        QSqlQuery& upsertAlbum = statements.album;
        upsertAlbum.addBindValue(QString::fromStdString(track.album->id));
        upsertAlbum.addBindValue(QString::fromStdString(track.album->title));
        upsertAlbum.addBindValue(track.album->artworkUrl
                                     ? QVariant(QString::fromStdString(*track.album->artworkUrl))
                                     : QVariant{});
        if (!upsertAlbum.exec()) {
            rollback(db_);
            return false;
        }
        upsertAlbum.finish();
        QSqlQuery& link = statements.linkAlbum;
        link.addBindValue(trackId);
        link.addBindValue(QString::fromStdString(track.album->id));
        if (!link.exec()) {
            rollback(db_);
            return false;
        }
        link.finish();
    }
    if (track.localPath) {
        const QFileInfo file(QString::fromStdString(*track.localPath));
        const QString canonicalPath = file.canonicalFilePath();
        if (!canonicalPath.isEmpty() && file.isFile()) {
            QSqlQuery& localFile = statements.localFile;
            localFile.addBindValue(canonicalPath);
            localFile.addBindValue(trackId);
            localFile.addBindValue(file.size());
            localFile.addBindValue(file.lastModified().toMSecsSinceEpoch());
            if (!localFile.exec()) {
                rollback(db_);
                return false;
            }
            localFile.finish();
        }
    }
    return true;
}

std::vector<domain::Track> Database::loadTracks() const {
    // Set-based hydration: one query per relation table merged in memory,
    // instead of two relation queries per track (the old N+1 pattern).
    std::vector<domain::Track> result;
    if (!isOpen()) return result;
    std::unordered_map<std::string, std::size_t> byId;
    {
        QSqlQuery query(db_);
        if (!query.exec(QStringLiteral("SELECT track_id,title,duration_ms,local_path,remote_url FROM tracks ORDER BY title"))) return result;
        while (query.next()) {
            domain::Track track;
            track.id = domain::TrackId(query.value(0).toString().toStdString());
            track.title = query.value(1).toString().toStdString();
            track.duration = std::chrono::milliseconds(query.value(2).toLongLong());
            if (!query.value(3).isNull()) track.localPath = query.value(3).toString().toStdString();
            if (!query.value(4).isNull()) track.remoteUrl = query.value(4).toString().toStdString();
            byId.emplace(track.id.value(), result.size());
            result.emplace_back(std::move(track));
        }
    }
    {
        QSqlQuery artists(db_);
        artists.prepare(QStringLiteral(
            "SELECT ta.track_id,a.artist_id,a.name FROM track_artists ta JOIN artists a ON a.artist_id=ta.artist_id "
            "ORDER BY ta.track_id,ta.ordinal"));
        if (artists.exec()) {
            while (artists.next()) {
                const auto track = byId.find(artists.value(0).toString().toStdString());
                if (track == byId.end()) continue;
                result[track->second].artists.push_back({artists.value(1).toString().toStdString(),
                                                  artists.value(2).toString().toStdString()});
            }
        }
    }
    {
        QSqlQuery albums(db_);
        albums.prepare(QStringLiteral(
            "SELECT ta.track_id,a.album_id,a.title,a.artwork_url FROM track_albums ta "
            "JOIN albums a ON a.album_id=ta.album_id"));
        if (albums.exec()) {
            while (albums.next()) {
                const auto track = byId.find(albums.value(0).toString().toStdString());
                if (track == byId.end()) continue;
                domain::Album value;
                value.id = albums.value(1).toString().toStdString();
                value.title = albums.value(2).toString().toStdString();
                if (!albums.value(3).isNull()) value.artworkUrl = albums.value(3).toString().toStdString();
                result[track->second].album = std::move(value);
            }
        }
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
    QSqlQuery aliases(db_);
    if(aliases.exec("SELECT path,size_bytes,modified_ms,hash,keeper_path FROM duplicate_aliases"))while(aliases.next()) {
        result.push_back({std::filesystem::path(aliases.value(0).toString().toStdWString()),
            static_cast<std::uintmax_t>(aliases.value(1).toULongLong()),aliases.value(2).toLongLong(),
            aliases.value(3).toString().toStdString(),std::filesystem::path(aliases.value(4).toString().toStdWString())});
    }
    QSqlQuery exclusions(db_);
    if(exclusions.exec("SELECT path FROM library_exclusions"))while(exclusions.next())
        result.push_back({std::filesystem::path(exclusions.value(0).toString().toStdWString()),0,0,{}, {},true});
    return result;
}

QVariantList Database::loadLibraryFolders() const {
    if (!isOpen()) return {};

    QVariantList folders;
    QSqlQuery query(db_);
    query.prepare(QStringLiteral("SELECT folder_id,path FROM library_folders ORDER BY folder_id"));
    if (!query.exec()) return {};
    while (query.next()) folders.push_back(QVariantList{query.value(0), query.value(1)});
    return folders;
}

bool Database::addLibraryFolder(const QString& path) {
    if (!isOpen()) return false;
    const QString normalized = normalizedLibraryFolder(path);
    if (normalized.isEmpty()) return false;

    QSqlQuery lookup(db_);
    lookup.prepare(QStringLiteral("SELECT 1 FROM library_folders WHERE path = ? COLLATE NOCASE"));
    lookup.addBindValue(normalized);
    if (!lookup.exec()) return false;
    if (lookup.next()) return false;

    QSqlQuery insert(db_);
    insert.prepare(QStringLiteral(
        "INSERT INTO library_folders(path, added_ms) VALUES (?, ?) "
        "ON CONFLICT(path) DO NOTHING"));
    insert.addBindValue(normalized);
    insert.addBindValue(QDateTime::currentMSecsSinceEpoch());
    if (!insert.exec()) return false;
    return insert.numRowsAffected() == 1;
}

bool Database::removeLibraryFolder(std::int64_t id, const QString& path) {
    if (!isOpen()) return false;
    const QString normalized = normalizedLibraryFolder(path);
    if (normalized.isEmpty()) return false;

    QSqlQuery transaction(db_);
    if (!transaction.exec(QStringLiteral("BEGIN IMMEDIATE"))) return false;

    QSqlQuery lookup(db_);
    lookup.prepare(QStringLiteral("SELECT folder_id,path FROM library_folders WHERE folder_id = ?"));
    lookup.addBindValue(id);
    if (!lookup.exec() || !lookup.next()) {
        rollback(db_);
        return false;
    }
    const QString storedPath = lookup.value(1).toString();
    if (QString::compare(storedPath, normalized, Qt::CaseInsensitive) != 0) {
        rollback(db_);
        return false;
    }

    QSqlQuery tracks(db_);
    tracks.prepare(QStringLiteral(
        "DELETE FROM tracks WHERE track_id IN ("
        "SELECT track_id FROM local_files WHERE canonical_path LIKE ? ESCAPE '\\')"));
    tracks.addBindValue(libraryPrefixPattern(storedPath));
    if (!tracks.exec()) {
        rollback(db_);
        return false;
    }

    QSqlQuery remove(db_);
    remove.prepare(QStringLiteral("DELETE FROM library_folders WHERE folder_id = ?"));
    remove.addBindValue(id);
    if (!remove.exec()) {
        rollback(db_);
        return false;
    }

    QSqlQuery commit(db_);
    if (!commit.exec(QStringLiteral("COMMIT"))) return false;
    return remove.numRowsAffected() > 0;
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

namespace listenfree::infrastructure::database {
bool Database::clearLibraryIndex() {
    if (!db_.transaction()) return false;
    QSqlQuery query(db_);
    if (!query.exec("DELETE FROM tracks") || !query.exec("DELETE FROM albums") || !query.exec("DELETE FROM artists")) { db_.rollback(); return false; }
    return db_.commit();
}

bool Database::restoreMissingRelations(std::span<const domain::Track> tracks) {
    if (!isOpen()) return false;
    QSqlQuery transaction(db_);
    if (!transaction.exec(QStringLiteral("BEGIN IMMEDIATE"))) return false;
    UpsertStatements statements(db_);
    statements.artist.prepare(QStringLiteral("INSERT OR IGNORE INTO artists(artist_id,name) VALUES(?,?)"));
    statements.album.prepare(QStringLiteral("INSERT OR IGNORE INTO albums(album_id,title,artwork_url) VALUES(?,?,?)"));
    const auto execute = [](QSqlQuery& query, const QVariantList& values) {
        for (const auto& value : values) query.addBindValue(value);
        const bool ok = query.exec(); query.finish(); return ok;
    };
    for (const auto& track : tracks) {
        const auto current = findTrack(track.id);
        if (!current) continue;
        const auto id = QString::fromStdString(track.id.value());
        if (current->artists.empty()) {
            int ordinal = 0;
            for (const auto& artist : track.artists) {
                if (artist.id.empty() || artist.name.empty()) continue;
                const auto artistId = QString::fromStdString(artist.id);
                if (!execute(statements.artist, {artistId, QString::fromStdString(artist.name)}) ||
                    !execute(statements.linkArtist, {id, artistId, ordinal++})) { rollback(db_); return false; }
            }
        }
        if (!current->album && track.album && !track.album->id.empty()) {
            const auto albumId = QString::fromStdString(track.album->id);
            if (!execute(statements.album, {albumId, QString::fromStdString(track.album->title),
                    track.album->artworkUrl ? QVariant(QString::fromStdString(*track.album->artworkUrl)) : QVariant{}}) ||
                !execute(statements.linkAlbum, {id, albumId})) { rollback(db_); return false; }
        }
    }
    if (!transaction.exec(QStringLiteral("COMMIT"))) { rollback(db_); return false; }
    return true;
}
bool Database::removeTrack(const QString& id) {
    QSqlQuery query(db_);
    query.prepare("DELETE FROM tracks WHERE track_id = ?");
    query.addBindValue(id);
    return query.exec();
}
bool Database::removeLocalTrack(const QString& path, bool excludeFromScan) {
    if(path.isEmpty() || !db_.transaction())return false;
    const QFileInfo file(path);
    const auto canonical = file.canonicalFilePath();
    const auto normalized = QDir::cleanPath(canonical.isEmpty()?file.absoluteFilePath():canonical);
    QSqlQuery query(db_);
    if(excludeFromScan) {
        query.prepare("INSERT OR IGNORE INTO library_exclusions(path) VALUES(?)");query.addBindValue(normalized);
        if(!query.exec()) { db_.rollback(); return false; }
    }
    query.prepare("DELETE FROM tracks WHERE replace(local_path,'\\','/')=? COLLATE NOCASE");
    query.addBindValue(normalized);
    if(!query.exec() || !db_.commit()) { db_.rollback(); return false; }
    return true;
}
bool Database::restoreExcludedFiles(const QStringList& roots) {
    if(!db_.transaction())return false;
    QSqlQuery query(db_);
    query.prepare("DELETE FROM library_exclusions WHERE path LIKE ? ESCAPE '\\'");
    for(const auto& root:roots) {
        const auto normalized = QDir::cleanPath(QFileInfo(root).absoluteFilePath());
        query.bindValue(0, libraryPrefixPattern(normalized));
        if(!query.exec()) { db_.rollback(); return false; }
    }
    if(!db_.commit()) { db_.rollback(); return false; }
    return true;
}
bool Database::pruneMissingLocalFiles(const QStringList& roots, bool recursive) {
    QStringList subtreePrefixes;
    QSet<QString> directDirectories;
    const auto registered = loadLibraryFolders();
    QStringList onlineParents;
    for(const auto& value:registered) {
        const auto parent=QDir::cleanPath(value.toList().value(1).toString());
        if(QFileInfo(parent).isDir())onlineParents.append(parent.endsWith('/') ? parent : parent+'/');
    }
    for(const auto& raw:roots) {
        const auto root=QDir::cleanPath(QFileInfo(raw).absoluteFilePath());
        const bool exists=QFileInfo(root).isDir();
        bool accessible=exists;
        // A removed subdirectory is safe to reconcile while its registered
        // root is online. A missing drive/root is not evidence of deletion.
        for(const auto& prefix:onlineParents)
            if(root.startsWith(prefix,Qt::CaseInsensitive))accessible=true;
        if(!accessible)continue;
        if(recursive || !exists)subtreePrefixes.append(root.endsWith('/') ? root : root+'/');
        else directDirectories.insert(root.toCaseFolded());
    }
    if(subtreePrefixes.isEmpty() && directDirectories.isEmpty())return true;
    QSqlQuery files(db_);
    if(!files.exec("SELECT t.track_id,coalesce(l.canonical_path,t.local_path) FROM tracks t "
                   "LEFT JOIN local_files l ON l.track_id=t.track_id WHERE t.local_path IS NOT NULL AND t.local_path<>''"))return false;
    QList<QPair<QString,QString>> missing;
    const auto absent=[](const QString& path) {
        std::error_code error;
        const auto status=std::filesystem::symlink_status(std::filesystem::path(path.toStdWString()),error);
        return status.type()==std::filesystem::file_type::not_found
            && (!error || error==std::errc::no_such_file_or_directory || error==std::errc::not_a_directory);
    };
    QHash<QString,bool> missingParents;
    while(files.next()) {
        const auto path=QDir::fromNativeSeparators(files.value(1).toString());
        const auto parent=QFileInfo(path).absolutePath();
        bool within=directDirectories.contains(parent.toCaseFolded());
        for(const auto& prefix:subtreePrefixes) {
            if(path.startsWith(prefix,Qt::CaseInsensitive)) { within=true;break; }
        }
        // At startup a deleted subtree has no watch left to report it. An
        // existing ancestor's scan also removes records under missing folders.
        if(!within && !directDirectories.isEmpty()) {
            auto ancestor=QFileInfo(parent).absolutePath();
            while(ancestor!=parent) {
                if(directDirectories.contains(ancestor.toCaseFolded())) {
                    if(!missingParents.contains(parent))missingParents.insert(parent,absent(parent));
                    within=missingParents.value(parent);break;
                }
                const auto next=QFileInfo(ancestor).absolutePath();
                if(next==ancestor)break;
                ancestor=next;
            }
        }
        if(within && absent(path))missing.emplaceBack(files.value(0).toString(),path);
    }
    files.finish();
    if(missing.isEmpty())return true;
    if(!db_.transaction())return false;
    QSqlQuery remove(db_);remove.prepare("DELETE FROM tracks WHERE track_id=? AND replace(local_path,'\\','/')=? COLLATE NOCASE");
    for(const auto& [id,path]:missing) {
        if(!absent(path))continue;
        remove.bindValue(0,id);remove.bindValue(1,path);
        if(!remove.exec()) { db_.rollback(); return false; }
    }
    if(!db_.commit()) { db_.rollback(); return false; }
    return true;
}
}
