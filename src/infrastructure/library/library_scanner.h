#pragma once

#include "application/ports.h"
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

    void start(const QStringList& roots, bool recursive = true);
    void cancel();

signals:
    void tracksFound(const QVector<QString>& paths);
    void finished();
    void failed(const QString& message);

private:
    QFutureWatcher<QVector<QString>> watcher_;
    std::shared_ptr<std::atomic_bool> cancelled_;
};

class BasicMetadataReader final : public application::IMetadataReader {
public:
    std::optional<domain::Track> read(const std::filesystem::path& path) override;
};

class LocalLibraryScannerAdapter final : public QObject, public application::ILocalLibraryScanner {
    Q_OBJECT
public:
    explicit LocalLibraryScannerAdapter(std::unique_ptr<application::IMetadataReader> metadataReader = {},
                                        QObject* parent = nullptr);
    ~LocalLibraryScannerAdapter() override;

    void start(const application::ScanRequest& request, std::function<void(domain::Track)> onTrack,
               std::function<void(std::string)> onError, application::CancelCallback cancelled) override;
    void cancel() override;

private:
    LibraryScanner scanner_;
    std::unique_ptr<application::IMetadataReader> metadataReader_;
    std::function<void(domain::Track)> onTrack_;
    std::function<void(std::string)> onError_;
    application::CancelCallback cancelled_;
};

} // namespace listenfree::infrastructure::library
