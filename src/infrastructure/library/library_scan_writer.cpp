#include "infrastructure/library/library_scan_writer.h"

#include "application/ports.h"
#include "domain/domain.h"
#include "infrastructure/database/database.h"
#include "infrastructure/database/repositories.h"

namespace listenfree::infrastructure::library {

LibraryScanWriter::LibraryScanWriter(QObject* parent) : QObject(parent) {
    thread_.setObjectName(QStringLiteral("listenfree-scan-writer"));
    acceptedGeneration_ = std::make_shared<std::atomic<quint64>>(1);
    moveToThread(&thread_);
    thread_.start();
}

LibraryScanWriter::~LibraryScanWriter() {
    thread_.quit();
    thread_.wait();
}

void LibraryScanWriter::initialize(const QString& databasePath) {
    // Queued onto the writer thread by design: QSqlDatabase connections must
    // be created and used in the same thread.
    databasePath_ = databasePath;
}

void LibraryScanWriter::setAcceptedGeneration(quint64 generation) {
    auto current = acceptedGeneration_->load(std::memory_order_acquire);
    while (generation > current &&
           !acceptedGeneration_->compare_exchange_weak(current, generation, std::memory_order_acq_rel)) {
    }
}

void LibraryScanWriter::commit(quint64 generation, std::shared_ptr<std::vector<domain::Track>> batch) {
    if (generation < acceptedGeneration_->load(std::memory_order_acquire)) {
        emit committed(generation, 0);
        return;
    }
    if (!database_) {
        database_ = std::make_unique<database::Database>();
        if (!database_->openExisting(databasePath_)) {
            emit commitFailed(generation, QStringLiteral("scan-writer-open-failed"));
            return;
        }
        repository_ = std::make_unique<database::TrackRepository>(*database_);
    }
    if (!repository_->upsert(*batch)) {
        emit commitFailed(generation, QStringLiteral("library.repository-upsert-failed"));
        return;
    }
    emit committed(generation, static_cast<int>(batch->size()));
}

void LibraryScanWriter::drain() {
    emit drained();
}

} // namespace listenfree::infrastructure::library
