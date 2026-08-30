#pragma once

#include "domain/domain.h"

#include <QObject>
#include <QFutureWatcher>
#include <QStringList>
#include <atomic>
#include <memory>

namespace listenfree::infrastructure::library {

class LibraryScanner final : public QObject {
    Q_OBJECT
public:
    explicit LibraryScanner(QObject* parent = nullptr);
    ~LibraryScanner() override;

    void start(const QStringList& roots);
    void cancel();

signals:
    void tracksFound(const QVector<QString>& paths);
    void finished();
    void failed(const QString& message);

private:
    QFutureWatcher<QVector<QString>> watcher_;
    std::shared_ptr<std::atomic_bool> cancelled_;
};

} // namespace listenfree::infrastructure::library
