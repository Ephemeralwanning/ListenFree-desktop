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

} // namespace listenfree::qmlbridge
