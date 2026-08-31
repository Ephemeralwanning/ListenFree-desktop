#pragma once

#include "application/application_facade.h"
#include "application/ports.h"
#include "qmlbridge/list_models.h"

#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QtGlobal>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

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
    Q_PROPERTY(quint64 importedCount READ importedCount NOTIFY importedCountChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY lastErrorChanged)
public:
    explicit LibraryController(application::ILocalLibraryScanner& scanner,
                               application::ITrackRepository& repository,
                               QObject* parent = nullptr);
    ~LibraryController() override;

    [[nodiscard]] bool scanning() const noexcept { return scanning_; }
    [[nodiscard]] quint64 importedCount() const noexcept { return importedCount_; }
    [[nodiscard]] QString lastError() const { return lastError_; }

    Q_INVOKABLE void scan(const QStringList& roots);
    Q_INVOKABLE void cancel();
signals:
    void scanningChanged();
    void importedCountChanged();
    void lastErrorChanged();
private:
    void handleBatch(std::uint64_t generation, std::vector<domain::Track> batch);
    void handleFinished(std::uint64_t generation, application::ScanOutcome outcome);
    void finish(std::uint64_t generation, application::ScanOutcome outcome);
    void setLastError(QString error);

    application::ILocalLibraryScanner& scanner_;
    application::ITrackRepository& repository_;
    std::optional<application::ScanId> activeScanId_;
    std::uint64_t generation_{0};
    quint64 importedCount_{0};
    QString lastError_;
    bool scanning_{false};
};

class PlayerController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(qint64 position READ position NOTIFY positionChanged)
    Q_PROPERTY(qint64 duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(bool seekable READ seekable NOTIFY seekableChanged)
    Q_PROPERTY(float volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(bool muted READ muted WRITE setMuted NOTIFY mutedChanged)
    Q_PROPERTY(quint32 capabilities READ capabilities NOTIFY capabilitiesChanged)
    Q_PROPERTY(QStringList deviceIds READ deviceIds NOTIFY devicesChanged)
    Q_PROPERTY(QString errorCode READ errorCode NOTIFY errorChanged)
    Q_PROPERTY(QString errorMessage READ errorMessage NOTIFY errorChanged)
    Q_PROPERTY(bool errorRetryable READ errorRetryable NOTIFY errorChanged)
    Q_PROPERTY(QString audioCodec READ audioCodec NOTIFY audioFormatChanged)
    Q_PROPERTY(int sampleRate READ sampleRate NOTIFY audioFormatChanged)
    Q_PROPERTY(int channelCount READ channelCount NOTIFY audioFormatChanged)
    Q_PROPERTY(QueueModel* queueModel READ queueModel CONSTANT)
    Q_PROPERTY(QString currentTrackId READ currentTrackId NOTIFY currentTrackChanged)
    Q_PROPERTY(int currentLyricIndex READ currentLyricIndex NOTIFY currentLyricChanged)
    Q_PROPERTY(QString currentLyricText READ currentLyricText NOTIFY currentLyricChanged)
    Q_PROPERTY(int lyricLineCount READ lyricLineCount NOTIFY lyricsChanged)
public:
    explicit PlayerController(QObject* parent = nullptr);
    explicit PlayerController(std::unique_ptr<application::IAudioPlayer> player,
                              QObject* parent = nullptr);
    ~PlayerController() override;
    [[nodiscard]] QString state() const;
    [[nodiscard]] qint64 position() const noexcept;
    [[nodiscard]] qint64 duration() const noexcept;
    [[nodiscard]] bool seekable() const noexcept;
    [[nodiscard]] float volume() const noexcept;
    [[nodiscard]] bool muted() const noexcept;
    [[nodiscard]] quint32 capabilities() const noexcept;
    [[nodiscard]] QStringList deviceIds() const;
    [[nodiscard]] QString errorCode() const;
    [[nodiscard]] QString errorMessage() const;
    [[nodiscard]] bool errorRetryable() const;
    [[nodiscard]] QString audioCodec() const;
    [[nodiscard]] int sampleRate() const;
    [[nodiscard]] int channelCount() const;
    [[nodiscard]] QueueModel* queueModel() const noexcept { return queueModel_.get(); }
    [[nodiscard]] QString currentTrackId() const;
    [[nodiscard]] int currentLyricIndex() const noexcept;
    [[nodiscard]] QString currentLyricText() const;
    [[nodiscard]] int lyricLineCount() const noexcept;
    Q_INVOKABLE void openLocal(const QString& path);
    Q_INVOKABLE void openUrl(const QUrl& url);
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void seek(qint64 position);
    Q_INVOKABLE void setVolume(float volume);
    Q_INVOKABLE void setMuted(bool muted);
    Q_INVOKABLE bool selectDevice(const QString& id);
    void setQueue(std::vector<domain::PlaybackItem> items, std::size_t currentIndex = 0);
    void setLyrics(std::vector<domain::LyricLine> lyrics);
signals:
    void stateChanged();
    void positionChanged();
    void durationChanged();
    void seekableChanged();
    void volumeChanged();
    void mutedChanged();
    void capabilitiesChanged();
    void devicesChanged();
    void errorChanged();
    void audioFormatChanged();
    void currentTrackChanged();
    void currentLyricChanged();
    void lyricsChanged();
private:
    class Impl;
    std::unique_ptr<QueueModel> queueModel_;
    std::unique_ptr<Impl> impl_;
    void syncQueueModel();
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
