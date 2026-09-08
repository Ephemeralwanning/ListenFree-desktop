#pragma once

#include "application/ports.h"

#include <QSqlDatabase>
#include <QSqlQuery>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <optional>
#include <string>
#include <vector>
#include <functional>

namespace listenfree::infrastructure::database {

// Prepared once per transaction and reused for every track, mirroring fooyin's
// TrackDatabase::storeTracks statement handling; per-track prepare+finalize was
// the dominant write cost for large scans.
struct UpsertStatements {
    QSqlQuery track;
    QSqlQuery clearArtists;
    QSqlQuery clearAlbums;
    QSqlQuery clearFiles;
    QSqlQuery artist;
    QSqlQuery linkArtist;
    QSqlQuery album;
    QSqlQuery linkAlbum;
    QSqlQuery localFile;

    explicit UpsertStatements(QSqlDatabase& database);
};

class Database final {
public:
    Database() = default;
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool open(const QString& path);
    // Opens a secondary connection to an already-migrated database file for a
    // worker thread (fooyin DbConnectionPool pattern): schema setup stays with
    // the primary connection; this one only applies per-connection pragmas.
    bool openExisting(const QString& path);
    void close() noexcept;
    [[nodiscard]] bool isOpen() const noexcept { return db_.isValid() && db_.isOpen(); }
    bool migrate();
    bool upsertTrack(const domain::Track& track);
    bool upsertTracks(std::span<const domain::Track> tracks);
    // Recheck missing relations under the write lock; never restore a deleted
    // track or overwrite an edit made while its tags were read in the worker.
    bool restoreMissingRelations(std::span<const domain::Track> tracks);
    [[nodiscard]] std::optional<domain::Track> findTrack(const domain::TrackId& id) const;
    [[nodiscard]] std::vector<domain::Track> searchTracks(const QString& queryText) const;
    [[nodiscard]] std::vector<domain::Track> loadTracks() const;
    [[nodiscard]] std::vector<application::LocalFileFingerprint> loadLocalFiles() const;
    [[nodiscard]] QVariantList loadLibraryFolders() const;
    bool addLibraryFolder(const QString& path);
    bool removeLibraryFolder(std::int64_t id, const QString& path);
    [[nodiscard]] std::vector<domain::Playlist> loadPlaylists() const;
    bool savePlaylist(const domain::Playlist& playlist);
    bool removePlaylist(const domain::PlaylistId& id);
    bool clearLibraryIndex();
    bool removeTrack(const QString& id);
    bool recordPlayHistory(const domain::TrackId& id,
                           std::chrono::system_clock::time_point when);
    [[nodiscard]] std::optional<std::string> getSetting(const QString& key) const;
    bool setSetting(const QString& key, const QString& value, const QString& valueType = QStringLiteral("string"));
    bool applySettings(const QVariantMap& values, const QStringList& addedRoots = {}, const std::function<bool()>& applyRuntime = {});
    bool clearScrollPositions();
    bool mergeDuplicate(const QVariantMap& duplicate, const QVariantMap& keeper, const QString& hash, bool alias);

private:
    bool connect(const QString& path);
    bool upsertTrackRows(const domain::Track& track);
    bool upsertTrackRows(const domain::Track& track, UpsertStatements& statements);

    QSqlDatabase db_;
    QString connectionName_;
};

} // namespace listenfree::infrastructure::database
