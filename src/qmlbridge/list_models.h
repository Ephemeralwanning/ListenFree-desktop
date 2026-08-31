#pragma once

#include "domain/domain.h"

#include <QAbstractListModel>
#include <vector>

namespace listenfree::qmlbridge {

class TrackListModel : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role { TrackIdRole = Qt::UserRole + 1, TitleRole, ArtistRole, DurationRole };
    explicit TrackListModel(QObject* parent = nullptr);
    void setTracks(std::vector<domain::Track> tracks);
    [[nodiscard]] const std::vector<domain::Track>& tracks() const noexcept { return tracks_; }
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
private:
    std::vector<domain::Track> tracks_;
};

class QueueModel final : public TrackListModel {
    Q_OBJECT
    Q_PROPERTY(int currentIndex READ currentIndex NOTIFY currentIndexChanged)
public:
    using TrackListModel::TrackListModel;
    [[nodiscard]] int currentIndex() const noexcept { return currentIndex_; }
    void setCurrentIndex(int index);
signals:
    void currentIndexChanged();
private:
    int currentIndex_{-1};
};

class PlaylistListModel final : public QAbstractListModel {
    Q_OBJECT
public:
    enum Role { PlaylistIdRole = Qt::UserRole + 1, TitleRole, EntryCountRole };
    explicit PlaylistListModel(QObject* parent = nullptr);
    void setPlaylists(std::vector<domain::Playlist> playlists);
    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;
private:
    std::vector<domain::Playlist> playlists_;
};

} // namespace listenfree::qmlbridge
