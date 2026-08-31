#include "media/qt_audio_player.h"

#include <QUrl>

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
    connect(&devices_, &QMediaDevices::audioOutputsChanged, this, &QtAudioPlayer::devicesChanged);
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

void QtAudioPlayer::open(const domain::PlaybackItem& item) {
    if (item.resolvedUrl) {
        open(QUrl(QString::fromStdString(*item.resolvedUrl)));
    } else if (item.track.localPath) {
        open(QUrl::fromLocalFile(QString::fromStdString(*item.track.localPath)));
    } else if (item.track.remoteUrl) {
        open(QUrl(QString::fromStdString(*item.track.remoteUrl)));
    } else {
        // An item without a local or remote source means nothing is loaded;
        // this is the normal idle state, not a playback failure.
        player_.setSource(QUrl{});
        if (state_ != domain::PlaybackState::Idle) {
            state_ = domain::PlaybackState::Idle;
            emit stateChanged();
        }
    }
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
void QtAudioPlayer::seek(std::chrono::milliseconds position) { seek(position.count()); }
void QtAudioPlayer::setVolume(float volume) { output_.setVolume(std::clamp(volume, 0.0F, 1.0F)); }

std::uint32_t QtAudioPlayer::capabilities() const noexcept {
    using application::PlaybackCapability;
    return application::capabilityMask(PlaybackCapability::LocalFile) |
           application::capabilityMask(PlaybackCapability::HttpStream) |
           application::capabilityMask(PlaybackCapability::Seek) |
           application::capabilityMask(PlaybackCapability::Volume) |
           application::capabilityMask(PlaybackCapability::Mute) |
           application::capabilityMask(PlaybackCapability::DeviceSelection);
}

std::vector<std::string> QtAudioPlayer::deviceIds() const {
    std::vector<std::string> result;
    const auto outputs = QMediaDevices::audioOutputs();
    result.reserve(static_cast<std::size_t>(outputs.size()));
    for (const auto& device : outputs) result.push_back(device.id().toStdString());
    return result;
}

QStringList QtAudioPlayer::qtDeviceIds() const {
    QStringList result;
    const auto outputs = QMediaDevices::audioOutputs();
    result.reserve(outputs.size());
    for (const auto& device : outputs) result.push_back(QString::fromUtf8(device.id()));
    return result;
}

bool QtAudioPlayer::select(std::string_view id) {
    const QByteArray requested(id.data(), static_cast<qsizetype>(id.size()));
    const auto outputs = QMediaDevices::audioOutputs();
    const auto match = std::find_if(outputs.cbegin(), outputs.cend(), [&requested](const QAudioDevice& device) {
        return device.id() == requested;
    });
    if (match == outputs.cend()) return false;
    output_.setDevice(*match);
    return true;
}

bool QtAudioPlayer::selectDevice(const QString& id) { return select(id.toStdString()); }

} // namespace listenfree::media
