#include "qmlbridge/controllers.h"

#include "application/playback_service.h"
#include "media/qt_audio_player.h"

#include <QPointer>

#include <exception>
#include <filesystem>
#include <span>
#include <stdexcept>

namespace listenfree::qmlbridge {

AppController::AppController(QObject* parent)
    : QObject(parent),
      facade_(nullptr),
      tracksModel_(std::make_unique<TrackListModel>()),
      queueModel_(std::make_unique<QueueModel>()) {
    connect(&facade_, &application::ApplicationFacade::readyChanged, this, [this] {
        tracksModel_->setTracks(facade_.tracks());
        std::vector<domain::Track> queueTracks;
        queueTracks.reserve(facade_.queue().items().size());
        for (const auto& item : facade_.queue().items()) queueTracks.push_back(item.track);
        queueModel_->setTracks(std::move(queueTracks));
        emit readyChanged();
        emit playbackStateChanged();
    });
}

QString AppController::playbackState() const {
    switch (facade_.playbackState()) {
    case domain::PlaybackState::Idle: return QStringLiteral("Idle");
    case domain::PlaybackState::Loading: return QStringLiteral("Loading");
    case domain::PlaybackState::Playing: return QStringLiteral("Playing");
    case domain::PlaybackState::Paused: return QStringLiteral("Paused");
    case domain::PlaybackState::Stopped: return QStringLiteral("Stopped");
    case domain::PlaybackState::Buffering: return QStringLiteral("Buffering");
    case domain::PlaybackState::Error: return QStringLiteral("Error");
    }
    return QStringLiteral("Error");
}

void AppController::initialize() { facade_.initializeMock(); }
void AppController::shutdown() {}

LibraryController::LibraryController(application::ILocalLibraryScanner& scanner,
                                     application::ITrackRepository& repository,
                                     QObject* parent)
    : QObject(parent), scanner_(scanner), repository_(repository) {}

LibraryController::~LibraryController() {
    ++generation_;
    const auto activeScanId = activeScanId_;
    activeScanId_.reset();
    scanning_ = false;
    if (activeScanId) scanner_.cancel(*activeScanId);
}

void LibraryController::scan(const QStringList& roots) {
    cancel();

    const auto generation = ++generation_;
    activeScanId_.reset();
    if (importedCount_ != 0) {
        importedCount_ = 0;
        emit importedCountChanged();
    }
    setLastError({});
    scanning_ = true;
    emit scanningChanged();

    application::ScanRequest request;
    request.roots.reserve(static_cast<std::size_t>(roots.size()));
    for (const auto& root : roots) {
        if (!root.isEmpty()) request.roots.emplace_back(root.toStdWString());
    }
    QPointer<LibraryController> guard(this);
    application::ScanCallbacks callbacks;
    callbacks.onBatch = [guard, generation](std::vector<domain::Track> batch) mutable {
        if (guard) guard->handleBatch(generation, std::move(batch));
    };
    callbacks.onFinished = [guard, generation](application::ScanOutcome outcome) mutable {
        if (guard) guard->handleFinished(generation, std::move(outcome));
    };

    try {
        request.knownFiles = repository_.localFiles();
        const auto scanId = scanner_.start(request, std::move(callbacks));
        if (generation == generation_ && scanning_) {
            activeScanId_ = scanId;
        } else {
            scanner_.cancel(scanId);
        }
    } catch (const std::exception& error) {
        finish(generation, {application::ScanStatus::Failed, error.what()});
    } catch (...) {
        finish(generation, {application::ScanStatus::Failed, "library.scan-start-failed"});
    }
}

void LibraryController::cancel() {
    if (!scanning_) return;
    const auto generation = generation_;
    const auto activeScanId = activeScanId_;
    finish(generation, {application::ScanStatus::Cancelled, {}});
    if (activeScanId) scanner_.cancel(*activeScanId);
}

void LibraryController::handleBatch(std::uint64_t generation, std::vector<domain::Track> batch) {
    if (generation != generation_ || !scanning_ || batch.empty()) return;

    if (!repository_.upsert(std::span<const domain::Track>(batch))) {
        const auto activeScanId = activeScanId_;
        finish(generation, {application::ScanStatus::Failed, "library.repository-upsert-failed"});
        if (activeScanId) scanner_.cancel(*activeScanId);
        return;
    }

    importedCount_ += static_cast<quint64>(batch.size());
    emit importedCountChanged();
}

void LibraryController::handleFinished(std::uint64_t generation, application::ScanOutcome outcome) {
    finish(generation, std::move(outcome));
}

void LibraryController::finish(std::uint64_t generation, application::ScanOutcome outcome) {
    if (generation != generation_ || !scanning_) return;

    activeScanId_.reset();
    if (outcome.status == application::ScanStatus::Failed) {
        setLastError(outcome.error.empty() ? QStringLiteral("library.scan-failed")
                                           : QString::fromStdString(outcome.error));
    }
    scanning_ = false;
    emit scanningChanged();
}

void LibraryController::setLastError(QString error) {
    if (lastError_ == error) return;
    lastError_ = std::move(error);
    emit lastErrorChanged();
}

class PlayerController::Impl final {
public:
    explicit Impl(std::unique_ptr<application::IAudioPlayer> ownedPlayer)
        : playerOwner(std::move(ownedPlayer)), player(requirePlayer(playerOwner)),
          backend(dynamic_cast<application::IPlaybackBackend*>(playerOwner.get())),
          devices(dynamic_cast<application::IAudioDeviceService*>(playerOwner.get())),
          playback(player) {}

    static application::IAudioPlayer& requirePlayer(
        const std::unique_ptr<application::IAudioPlayer>& candidate) {
        if (!candidate) throw std::invalid_argument("PlayerController requires an audio player");
        return *candidate;
    }

    std::unique_ptr<application::IAudioPlayer> playerOwner;
    application::IAudioPlayer& player;
    application::IPlaybackBackend* backend;
    application::IAudioDeviceService* devices;
    application::PlaybackService playback{player};
};

namespace {

QString playbackStateName(domain::PlaybackState state) {
    switch (state) {
    case domain::PlaybackState::Idle: return QStringLiteral("Idle");
    case domain::PlaybackState::Loading: return QStringLiteral("Loading");
    case domain::PlaybackState::Playing: return QStringLiteral("Playing");
    case domain::PlaybackState::Paused: return QStringLiteral("Paused");
    case domain::PlaybackState::Stopped: return QStringLiteral("Stopped");
    case domain::PlaybackState::Buffering: return QStringLiteral("Buffering");
    case domain::PlaybackState::Error: return QStringLiteral("Error");
    }
    return QStringLiteral("Error");
}

} // namespace

PlayerController::PlayerController(QObject* parent)
    : PlayerController(std::make_unique<media::QtAudioPlayer>(), parent) {}

PlayerController::PlayerController(std::unique_ptr<application::IAudioPlayer> player, QObject* parent)
    : QObject(parent), queueModel_(std::make_unique<QueueModel>()),
      impl_(std::make_unique<Impl>(std::move(player))) {
    application::PlaybackServiceEvents events;
    events.onStateChanged = [this] { emit stateChanged(); };
    events.onPositionChanged = [this] { emit positionChanged(); };
    events.onDurationChanged = [this] { emit durationChanged(); };
    events.onSeekableChanged = [this] { emit seekableChanged(); };
    events.onVolumeChanged = [this] { emit volumeChanged(); };
    events.onMutedChanged = [this] { emit mutedChanged(); };
    events.onErrorChanged = [this] { emit errorChanged(); };
    events.onAudioFormatChanged = [this] { emit audioFormatChanged(); };
    events.onQueueChanged = [this] { syncQueueModel(); };
    events.onCurrentItemChanged = [this] { emit currentTrackChanged(); };
    events.onLyricsChanged = [this] { emit lyricsChanged(); };
    events.onCurrentLyricChanged = [this] { emit currentLyricChanged(); };
    impl_->playback.setEvents(std::move(events));

    application::PlaybackBackendEvents backendEvents;
    backendEvents.onCapabilitiesChanged = [this](std::uint32_t) { emit capabilitiesChanged(); };
    if (impl_->backend) impl_->backend->setBackendEvents(std::move(backendEvents));
    application::AudioDeviceEvents deviceEvents;
    deviceEvents.onDevicesChanged = [this] { emit devicesChanged(); };
    deviceEvents.onSelectedDeviceChanged = [this](std::string) { emit devicesChanged(); };
    if (impl_->devices) impl_->devices->setDeviceEvents(std::move(deviceEvents));
}

PlayerController::~PlayerController() {
    if (impl_->backend) impl_->backend->setBackendEvents({});
    if (impl_->devices) impl_->devices->setDeviceEvents({});
}

QString PlayerController::state() const { return playbackStateName(impl_->player.state()); }
qint64 PlayerController::position() const noexcept { return impl_->player.position().count(); }
qint64 PlayerController::duration() const noexcept { return impl_->player.duration().count(); }
bool PlayerController::seekable() const noexcept { return impl_->player.seekable(); }
float PlayerController::volume() const noexcept { return impl_->player.volume(); }
bool PlayerController::muted() const noexcept { return impl_->player.muted(); }
quint32 PlayerController::capabilities() const noexcept {
    return impl_->backend ? impl_->backend->capabilities() : 0U;
}
QStringList PlayerController::deviceIds() const {
    QStringList result;
    if (!impl_->devices) return result;
    for (const auto& device : impl_->devices->devices()) {
        result.push_back(QString::fromStdString(device.id));
    }
    return result;
}

QString PlayerController::errorCode() const {
    const auto error = impl_->player.lastError();
    if (!error) return {};
    switch (error->code) {
    case domain::PlaybackErrorCode::InvalidTransition: return QStringLiteral("invalid-transition");
    case domain::PlaybackErrorCode::OpenFailed: return QStringLiteral("open-failed");
    case domain::PlaybackErrorCode::Network: return QStringLiteral("network");
    case domain::PlaybackErrorCode::Unsupported: return QStringLiteral("unsupported");
    case domain::PlaybackErrorCode::Internal: return QStringLiteral("internal");
    }
    return QStringLiteral("internal");
}

QString PlayerController::errorMessage() const {
    const auto error = impl_->player.lastError();
    return error ? QString::fromStdString(error->message) : QString{};
}

bool PlayerController::errorRetryable() const {
    const auto error = impl_->player.lastError();
    return error && error->retryable;
}

QString PlayerController::audioCodec() const {
    const auto format = impl_->player.audioFormat();
    return format ? QString::fromStdString(format->codec) : QString{};
}

int PlayerController::sampleRate() const {
    const auto format = impl_->player.audioFormat();
    return format ? format->sampleRate : 0;
}

int PlayerController::channelCount() const {
    const auto format = impl_->player.audioFormat();
    return format ? format->channels : 0;
}

QString PlayerController::currentTrackId() const {
    const auto* item = impl_->playback.currentItem();
    return item ? QString::fromStdString(item->track.id.value()) : QString{};
}

int PlayerController::currentLyricIndex() const noexcept {
    const auto index = impl_->playback.currentLyricIndex();
    return index ? static_cast<int>(*index) : -1;
}

QString PlayerController::currentLyricText() const {
    const auto line = impl_->playback.currentLyricLine();
    return line ? QString::fromStdString(line->text) : QString{};
}

int PlayerController::lyricLineCount() const noexcept {
    return static_cast<int>(impl_->playback.lyrics().size());
}

void PlayerController::openLocal(const QString& path) {
    if (path.isEmpty()) {
        setQueue({});
        return;
    }
    domain::PlaybackItem item;
    item.track.localPath = path.toStdString();
    setQueue({item});
    impl_->player.open(item);
}

void PlayerController::openUrl(const QUrl& url) {
    if (url.isEmpty()) {
        setQueue({});
        return;
    }
    domain::PlaybackItem item;
    item.resolvedUrl = url.toString().toStdString();
    setQueue({item});
    impl_->player.open(item);
}

void PlayerController::play() {
    if (impl_->player.state() == domain::PlaybackState::Idle && impl_->playback.currentItem()) {
        impl_->playback.playCurrent();
    } else {
        impl_->player.play();
    }
}
void PlayerController::pause() { impl_->player.pause(); }
void PlayerController::stop() { impl_->player.stop(); }
void PlayerController::seek(qint64 position) {
    impl_->player.seek(std::chrono::milliseconds(position));
}
void PlayerController::setVolume(float volume) { impl_->player.setVolume(volume); }
void PlayerController::setMuted(bool muted) { impl_->player.setMuted(muted); }
bool PlayerController::selectDevice(const QString& id) {
    return impl_->devices && impl_->devices->select(id.toStdString());
}

void PlayerController::setQueue(std::vector<domain::PlaybackItem> items, std::size_t currentIndex) {
    impl_->playback.setQueue(std::move(items), currentIndex);
}

void PlayerController::setLyrics(std::vector<domain::LyricLine> lyrics) {
    impl_->playback.setLyrics(std::move(lyrics));
}

void PlayerController::syncQueueModel() {
    std::vector<domain::Track> tracks;
    tracks.reserve(impl_->playback.queue().items().size());
    for (const auto& item : impl_->playback.queue().items()) tracks.push_back(item.track);
    queueModel_->setTracks(std::move(tracks));
    queueModel_->setCurrentIndex(impl_->playback.queue().empty()
                                     ? -1
                                     : static_cast<int>(impl_->playback.queue().currentIndex()));
}

} // namespace listenfree::qmlbridge
