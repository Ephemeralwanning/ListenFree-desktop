#include "infrastructure/database/repositories.h"

#include <QString>

namespace listenfree::infrastructure::database {

bool TrackRepository::upsert(std::span<const domain::Track> tracks) {
    for (const auto& track : tracks) {
        if (!database_.upsertTrack(track)) return false;
    }
    return true;
}

std::optional<domain::Track> TrackRepository::find(const domain::TrackId& id) {
    return database_.findTrack(id);
}

std::vector<domain::Track> TrackRepository::search(const std::string& query) {
    return database_.searchTracks(QString::fromStdString(query));
}

std::optional<std::string> SettingsRepository::get(const std::string& key) {
    return database_.getSetting(QString::fromStdString(key));
}

bool SettingsRepository::set(const std::string& key, const std::string& value) {
    return database_.setSetting(QString::fromStdString(key), QString::fromStdString(value));
}

} // namespace listenfree::infrastructure::database
