#pragma once

#include "domain/domain.h"

#include <QSqlDatabase>
#include <QString>
#include <optional>
#include <string>
#include <vector>

namespace listenfree::infrastructure::database {

class Database final {
public:
    Database() = default;
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool open(const QString& path);
    void close() noexcept;
    [[nodiscard]] bool isOpen() const noexcept { return db_.isValid() && db_.isOpen(); }
    bool migrate();
    bool upsertTrack(const domain::Track& track);
    [[nodiscard]] std::optional<domain::Track> findTrack(const domain::TrackId& id) const;
    [[nodiscard]] std::vector<domain::Track> searchTracks(const QString& queryText) const;
    [[nodiscard]] std::vector<domain::Track> loadTracks() const;
    [[nodiscard]] std::vector<domain::Playlist> loadPlaylists() const;
    bool savePlaylist(const domain::Playlist& playlist);
    bool removePlaylist(const domain::PlaylistId& id);
    [[nodiscard]] std::optional<std::string> getSetting(const QString& key) const;
    bool setSetting(const QString& key, const QString& value, const QString& valueType = QStringLiteral("string"));

private:
    QSqlDatabase db_;
    QString connectionName_;
};

} // namespace listenfree::infrastructure::database
