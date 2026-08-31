#include "infrastructure/database/repositories.h"

#include <QString>

namespace listenfree::infrastructure::database {

bool TrackRepository::upsert(std::span<const domain::Track> tracks) {
    return database_.upsertTracks(tracks);
}

std::optional<domain::Track> TrackRepository::find(const domain::TrackId& id) {
    return database_.findTrack(id);
}

std::vector<domain::Track> TrackRepository::search(const std::string& query) {
    return database_.searchTracks(QString::fromStdString(query));
}

std::vector<application::LocalFileFingerprint> TrackRepository::localFiles() {
    return database_.loadLocalFiles();
}

std::optional<std::string> SettingsRepository::get(const std::string& key) {
    return database_.getSetting(QString::fromStdString(key));
}

bool SettingsRepository::set(const std::string& key, const std::string& value) {
    return database_.setSetting(QString::fromStdString(key), QString::fromStdString(value));
}

std::vector<domain::Playlist> PlaylistRepository::list() { return database_.loadPlaylists(); }

bool PlaylistRepository::save(const domain::Playlist& playlist) { return database_.savePlaylist(playlist); }

bool PlaylistRepository::remove(const domain::PlaylistId& id) { return database_.removePlaylist(id); }

bool PlayHistoryRepository::record(const domain::TrackId& id,
                                   std::chrono::system_clock::time_point when) {
    return database_.recordPlayHistory(id, when);
}

} // namespace listenfree::infrastructure::database
