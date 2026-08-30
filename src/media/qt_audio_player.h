#pragma once

#include "application/ports.h"

#include <QAudioOutput>
#include <QMediaPlayer>
#include <QObject>

namespace listenfree::media {

class QtAudioPlayer final : public QObject, public application::IAudioPlayer {
    Q_OBJECT
    Q_PROPERTY(QString state READ stateName NOTIFY stateChanged)
    Q_PROPERTY(qint64 position READ position NOTIFY positionChanged)
    Q_PROPERTY(qint64 duration READ duration NOTIFY durationChanged)
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

    Q_INVOKABLE void open(const QUrl& url);
    Q_INVOKABLE void seek(qint64 position);

signals:
    void stateChanged();
    void positionChanged();
    void durationChanged();
    void errorChanged(const QString& message);

private:
    QMediaPlayer player_;
    QAudioOutput output_;
    domain::PlaybackState state_{domain::PlaybackState::Idle};
};

} // namespace listenfree::media
