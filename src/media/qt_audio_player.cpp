#include "media/qt_audio_player.h"

#include <algorithm>

namespace listenfree::media {

QtAudioPlayer::QtAudioPlayer(QObject* parent) : QObject(parent) {
    player_.setAudioOutput(&output_);
    connect(&player_, &QMediaPlayer::positionChanged, this, &QtAudioPlayer::positionChanged);
    connect(&player_, &QMediaPlayer::durationChanged, this, &QtAudioPlayer::durationChanged);
    connect(&player_, &QMediaPlayer::playbackStateChanged, this, [this](QMediaPlayer::PlaybackState state) {
        const auto next = state == QMediaPlayer::PlayingState ? domain::PlaybackState::Playing
                       : state == QMediaPlayer::PausedState ? domain::PlaybackState::Paused
                                                             : domain::PlaybackState::Stopped;
        if (state_ != next) {
            state_ = next;
            emit stateChanged();
        }
    });
    connect(&player_, &QMediaPlayer::errorOccurred, this, [this](QMediaPlayer::Error, const QString& message) {
        state_ = domain::PlaybackState::Error;
        emit stateChanged();
        emit errorChanged(message);
    });
}

QString QtAudioPlayer::stateName() const {
    switch (state_) {
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

void QtAudioPlayer::open(const QUrl& url) {
    state_ = domain::PlaybackState::Loading;
    emit stateChanged();
    player_.setSource(url);
}

void QtAudioPlayer::play() { player_.play(); }
void QtAudioPlayer::pause() { player_.pause(); }
void QtAudioPlayer::stop() { player_.stop(); }
void QtAudioPlayer::seek(qint64 position) { player_.setPosition(position); }
void QtAudioPlayer::setVolume(float volume) { output_.setVolume(std::clamp(volume, 0.0F, 1.0F)); }

} // namespace listenfree::media
