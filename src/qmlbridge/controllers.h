#pragma once

#include "application/application_facade.h"
#include "qmlbridge/list_models.h"

#include <QObject>
#include <QString>
#include <memory>

namespace listenfree::qmlbridge {

class AppController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool ready READ ready NOTIFY readyChanged)
    Q_PROPERTY(bool mockMode READ mockMode CONSTANT)
    Q_PROPERTY(TrackListModel* tracksModel READ tracksModel CONSTANT)
    Q_PROPERTY(QueueModel* queueModel READ queueModel CONSTANT)
    Q_PROPERTY(QString playbackState READ playbackState NOTIFY playbackStateChanged)
public:
    explicit AppController(QObject* parent = nullptr);
    [[nodiscard]] bool ready() const noexcept { return facade_.ready(); }
    [[nodiscard]] bool mockMode() const noexcept { return true; }
    [[nodiscard]] TrackListModel* tracksModel() const noexcept { return tracksModel_.get(); }
    [[nodiscard]] QueueModel* queueModel() const noexcept { return queueModel_.get(); }
    [[nodiscard]] QString playbackState() const;
    Q_INVOKABLE void initialize();
    Q_INVOKABLE void shutdown();
signals:
    void readyChanged();
    void playbackStateChanged();
private:
    application::ApplicationFacade facade_;
    std::unique_ptr<TrackListModel> tracksModel_;
    std::unique_ptr<QueueModel> queueModel_;
};

class LibraryController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool scanning READ scanning NOTIFY scanningChanged)
public:
    explicit LibraryController(QObject* parent = nullptr) : QObject(parent) {}
    [[nodiscard]] bool scanning() const noexcept { return false; }
signals:
    void scanningChanged();
};

class PlayerController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
public:
    explicit PlayerController(QObject* parent = nullptr) : QObject(parent) {}
    [[nodiscard]] QString state() const { return QStringLiteral("Idle"); }
signals:
    void stateChanged();
};

class PlaylistController final : public QObject {
    Q_OBJECT
public:
    explicit PlaylistController(QObject* parent = nullptr) : QObject(parent) {}
};

class OnlineController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool providerEnabled READ providerEnabled CONSTANT)
public:
    explicit OnlineController(QObject* parent = nullptr) : QObject(parent) {}
    [[nodiscard]] bool providerEnabled() const noexcept { return true; }
};

class SettingsController final : public QObject {
    Q_OBJECT
public:
    explicit SettingsController(QObject* parent = nullptr) : QObject(parent) {}
};

} // namespace listenfree::qmlbridge
