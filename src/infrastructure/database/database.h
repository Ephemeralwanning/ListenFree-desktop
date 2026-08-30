#pragma once

#include "domain/domain.h"

#include <QSqlDatabase>
#include <QString>
#include <vector>

namespace listenfree::infrastructure::database {

class Database final {
public:
    Database() = default;
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    bool open(const QString& path);
    void close() noexcept;
    [[nodiscard]] bool isOpen() const noexcept { return db_.isValid() && db_.isOpen(); }
    bool migrate();
    bool upsertTrack(const domain::Track& track);
    [[nodiscard]] std::vector<domain::Track> loadTracks() const;

private:
    QSqlDatabase db_;
    QString connectionName_;
};

} // namespace listenfree::infrastructure::database
