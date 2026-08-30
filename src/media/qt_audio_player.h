#pragma once

#include "domain/domain.h"

#include <QAudioOutput>
#include <QMediaPlayer>
#include <QObject>

namespace listenfree::media {

class QtAudioPlayer final : public QObject {
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

    Q_INVOKABLE void open(const QUrl& url);
    Q_INVOKABLE void play();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void seek(qint64 position);
    Q_INVOKABLE void setVolume(float volume);

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
