#include "qmlbridge/list_models.h"

#include <QString>

namespace listenfree::qmlbridge {

TrackListModel::TrackListModel(QObject* parent) : QAbstractListModel(parent) {}

void TrackListModel::setTracks(std::vector<domain::Track> tracks) {
    beginResetModel();
    tracks_ = std::move(tracks);
    endResetModel();
}

int TrackListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(tracks_.size());
}

QVariant TrackListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) return {};
    const auto& track = tracks_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case Qt::DisplayRole:
    case TitleRole: return QString::fromStdString(track.title);
    case TrackIdRole: return QString::fromStdString(track.id.value());
    case ArtistRole: return track.artists.empty() ? QString{} : QString::fromStdString(track.artists.front().name);
    case DurationRole: return static_cast<qint64>(track.duration.count());
    default: return {};
    }
}

QHash<int, QByteArray> TrackListModel::roleNames() const {
    return {{TrackIdRole, "trackId"}, {TitleRole, "title"}, {ArtistRole, "artist"}, {DurationRole, "duration"}};
}

void QueueModel::setCurrentIndex(int index) {
    const int bounded = index >= 0 && index < rowCount() ? index : -1;
    if (currentIndex_ == bounded) return;
    currentIndex_ = bounded;
    emit currentIndexChanged();
}

PlaylistListModel::PlaylistListModel(QObject* parent) : QAbstractListModel(parent) {}

void PlaylistListModel::setPlaylists(std::vector<domain::Playlist> playlists) {
    beginResetModel();
    playlists_ = std::move(playlists);
    endResetModel();
}

int PlaylistListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : static_cast<int>(playlists_.size());
}

QVariant PlaylistListModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rowCount()) return {};
    const auto& playlist = playlists_[static_cast<std::size_t>(index.row())];
    switch (role) {
    case Qt::DisplayRole:
    case TitleRole: return QString::fromStdString(playlist.title);
    case PlaylistIdRole: return QString::fromStdString(playlist.id.value());
    case EntryCountRole: return static_cast<qint64>(playlist.entries.size());
    default: return {};
    }
}

QHash<int, QByteArray> PlaylistListModel::roleNames() const {
    return {{PlaylistIdRole, "playlistId"}, {TitleRole, "title"}, {EntryCountRole, "entryCount"}};
}

} // namespace listenfree::qmlbridge
