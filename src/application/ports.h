#pragma once

#include "domain/domain.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace listenfree::application {

enum class PlaybackCapability : std::uint32_t {
    LocalFile = 1U << 0U,
    HttpStream = 1U << 1U,
    Seek = 1U << 2U,
    Volume = 1U << 3U,
    Mute = 1U << 4U,
    DeviceSelection = 1U << 5U,
    Equalizer = 1U << 6U,
    Gapless = 1U << 7U,
    Crossfade = 1U << 8U,
    ReplayGain = 1U << 9U
};

constexpr std::uint32_t capabilityMask(PlaybackCapability capability) noexcept {
    return static_cast<std::uint32_t>(capability);
}

struct ScanRequest {
    std::vector<std::filesystem::path> roots;
    bool recursive{true};
};

using CancelCallback = std::function<bool()>;

class ITrackRepository {
public:
    virtual ~ITrackRepository() = default;
    virtual bool upsert(std::span<const domain::Track> tracks) = 0;
    virtual std::optional<domain::Track> find(const domain::TrackId& id) = 0;
    virtual std::vector<domain::Track> search(const std::string& query) = 0;
};

class IPlaylistRepository {
public:
    virtual ~IPlaylistRepository() = default;
    virtual std::vector<domain::Playlist> list() = 0;
    virtual bool save(const domain::Playlist& playlist) = 0;
    virtual bool remove(const domain::PlaylistId& id) = 0;
};

class ISettingsRepository {
public:
    virtual ~ISettingsRepository() = default;
    virtual std::optional<std::string> get(const std::string& key) = 0;
    virtual bool set(const std::string& key, const std::string& value) = 0;
};

class IPlayHistoryRepository {
public:
    virtual ~IPlayHistoryRepository() = default;
    virtual bool record(const domain::TrackId& id, std::chrono::system_clock::time_point when) = 0;
};

class ILocalLibraryScanner {
public:
    virtual ~ILocalLibraryScanner() = default;
    virtual void start(const ScanRequest&, std::function<void(domain::Track)> onTrack,
                       std::function<void(std::string)> onError, CancelCallback cancelled) = 0;
    virtual void cancel() = 0;
};

class IMetadataReader {
public:
    virtual ~IMetadataReader() = default;
    virtual std::optional<domain::Track> read(const std::filesystem::path& path) = 0;
};

class IAudioPlayer {
public:
    virtual ~IAudioPlayer() = default;
    virtual void open(const domain::PlaybackItem&) = 0;
    virtual void play() = 0;
    virtual void pause() = 0;
    virtual void stop() = 0;
    virtual void seek(std::chrono::milliseconds) = 0;
    virtual void setVolume(float normalized) = 0;
};

class IPlaybackBackend {
public:
    virtual ~IPlaybackBackend() = default;
    virtual std::uint32_t capabilities() const noexcept = 0;
};

class IAudioDeviceService {
public:
    virtual ~IAudioDeviceService() = default;
    virtual std::vector<std::string> deviceIds() const = 0;
    virtual bool select(std::string_view id) = 0;
};

class IEqualizerService {
public:
    virtual ~IEqualizerService() = default;
    virtual bool supported() const noexcept = 0;
};

class IOnlineProvider {
public:
    virtual ~IOnlineProvider() = default;
    virtual domain::ProviderId id() const = 0;
    virtual std::vector<domain::Track> search(const std::string& query) const = 0;
    virtual std::vector<domain::Playlist> playlists() const = 0;
};

class ISourceHostClient {
public:
    virtual ~ISourceHostClient() = default;
    virtual bool start() = 0;
    virtual void stop() noexcept = 0;
    virtual bool loadPlugin(const std::filesystem::path& path) = 0;
    virtual void cancel(const std::string& requestId) = 0;
};

} // namespace listenfree::application
