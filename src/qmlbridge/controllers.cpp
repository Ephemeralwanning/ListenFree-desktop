#include "qmlbridge/controllers.h"

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
