#pragma once

#include "application/ports.h"

#include <QAudioDevice>
#include <QAudioOutput>
#include <QMediaDevices>
#include <QMediaPlayer>
#include <QObject>

namespace listenfree::media {

class QtAudioPlayer final : public QObject,
                            public application::IAudioPlayer,
                            public application::IPlaybackBackend,
                            public application::IAudioDeviceService,
                            public application::IEqualizerService {
    Q_OBJECT
    Q_PROPERTY(QString state READ stateName NOTIFY stateChanged)
    Q_PROPERTY(qint64 position READ position NOTIFY positionChanged)
    Q_PROPERTY(qint64 duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(QStringList deviceIds READ qtDeviceIds NOTIFY devicesChanged)
    Q_PROPERTY(quint32 capabilities READ capabilities CONSTANT)
public:
    explicit QtAudioPlayer(QObject* parent = nullptr);
    [[nodiscard]] domain::PlaybackState state() const noexcept { return state_; }
    [[nodiscard]] QString stateName() const;
    [[nodiscard]] qint64 position() const noexcept { return player_.position(); }
    [[nodiscard]] qint64 duration() const noexcept { return player_.duration(); }

    void open(const domain::PlaybackItem& item) override;
    Q_INVOKABLE void play() override;
    Q_INVOKABLE void pause() override;
    Q_INVOKABLE void stop() override;
    void seek(std::chrono::milliseconds position) override;
    Q_INVOKABLE void setVolume(float volume) override;
    [[nodiscard]] std::uint32_t capabilities() const noexcept override;
    [[nodiscard]] std::vector<std::string> deviceIds() const override;
    bool select(std::string_view id) override;
    [[nodiscard]] bool supported() const noexcept override { return false; }
    [[nodiscard]] QStringList qtDeviceIds() const;

    Q_INVOKABLE void open(const QUrl& url);
    Q_INVOKABLE void seek(qint64 position);
    Q_INVOKABLE bool selectDevice(const QString& id);

signals:
    void stateChanged();
    void positionChanged();
    void durationChanged();
    void errorChanged(const QString& message);
    void devicesChanged();

private:
    QMediaPlayer player_;
    QAudioOutput output_;
    QMediaDevices devices_;
    domain::PlaybackState state_{domain::PlaybackState::Idle};
};

} // namespace listenfree::media
