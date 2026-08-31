#include "qmlbridge/controllers.h"

#include <QPointer>

#include <exception>
#include <filesystem>
#include <span>

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

PlayerController::PlayerController(QObject* parent)
    : QObject(parent), player_(std::make_unique<media::QtAudioPlayer>()) {
    connect(player_.get(), &media::QtAudioPlayer::stateChanged, this, &PlayerController::stateChanged);
    connect(player_.get(), &media::QtAudioPlayer::positionChanged, this, &PlayerController::positionChanged);
    connect(player_.get(), &media::QtAudioPlayer::durationChanged, this, &PlayerController::durationChanged);
    connect(player_.get(), &media::QtAudioPlayer::errorChanged, this, &PlayerController::errorChanged);
}

QString PlayerController::state() const { return player_->stateName(); }
qint64 PlayerController::position() const noexcept { return player_->position(); }
qint64 PlayerController::duration() const noexcept { return player_->duration(); }

void PlayerController::openLocal(const QString& path) {
    if (path.isEmpty()) {
        player_->open(domain::PlaybackItem{});
        return;
    }
    domain::PlaybackItem item;
    item.track.localPath = path.toStdString();
    player_->open(item);
}

void PlayerController::openUrl(const QUrl& url) { player_->open(url); }
void PlayerController::play() { player_->play(); }
void PlayerController::pause() { player_->pause(); }
void PlayerController::stop() { player_->stop(); }
void PlayerController::seek(qint64 position) { player_->seek(position); }
void PlayerController::setVolume(float volume) { player_->setVolume(volume); }

} // namespace listenfree::qmlbridge
