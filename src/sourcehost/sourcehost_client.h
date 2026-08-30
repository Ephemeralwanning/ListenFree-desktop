#pragma once

#include "application/ports.h"

#include <QProcess>
#include <QObject>

namespace listenfree::sourcehost {

class SourceHostClient final : public QObject, public application::ISourceHostClient {
    Q_OBJECT
public:
    explicit SourceHostClient(QString executablePath, QObject* parent = nullptr);
    ~SourceHostClient() override;

    bool start() override;
    void stop() noexcept override;
    bool loadPlugin(const std::filesystem::path& path) override;
    void cancel(const std::string& requestId) override;
    [[nodiscard]] bool running() const noexcept { return process_.state() != QProcess::NotRunning; }

signals:
    void ready();
    void crashed();
    void protocolError(const QString& message);

private:
    QString executablePath_;
    QProcess process_;
};

} // namespace listenfree::sourcehost

