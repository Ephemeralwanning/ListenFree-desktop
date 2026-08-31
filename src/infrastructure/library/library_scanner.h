#pragma once

#include "application/ports.h"
#include "domain/domain.h"

#include <QObject>
#include <QFutureWatcher>
#include <QStringList>
#include <atomic>
#include <cstdint>
#include <memory>

namespace listenfree::infrastructure::library {

struct ScanBatch {
    QVector<domain::Track> tracks;
    QString error;
    std::uint64_t generation{0};
};

using ScanBatchPtr = std::shared_ptr<ScanBatch>;

class LibraryScanner final : public QObject {
    Q_OBJECT
public:
    explicit LibraryScanner(QObject* parent = nullptr);
    ~LibraryScanner() override;

    void start(const QStringList& roots, std::shared_ptr<application::IMetadataReader> metadataReader,
               bool recursive = true);
    void cancel();

signals:
    void tracksFound(QVector<domain::Track> tracks);
    void finished();
    void failed(const QString& message);

private:
    QFutureWatcher<ScanBatchPtr> watcher_;
    std::shared_ptr<std::atomic_bool> cancelled_;
    std::uint64_t generation_{0};
};

class BasicMetadataReader final : public application::IMetadataReader {
public:
    std::optional<domain::Track> read(const std::filesystem::path& path) override;
};

class TagLibMetadataReader final : public application::IMetadataReader {
public:
    std::optional<domain::Track> read(const std::filesystem::path& path) override;

private:
    BasicMetadataReader fallback_;
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
    std::shared_ptr<application::IMetadataReader> metadataReader_;
    std::function<void(domain::Track)> onTrack_;
    std::function<void(std::string)> onError_;
    application::CancelCallback cancelled_;
};

} // namespace listenfree::infrastructure::library
