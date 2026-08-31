#pragma once

#include "application/ports.h"
#include "sourcehost/source_protocol.h"

#include <QByteArray>
#include <QHash>
#include <QPointer>
#include <QProcess>
#include <QObject>
#include <QTimer>

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
    bool request(const SourceMessage& message, int timeoutMs = 5000);
    void setAutoRestart(bool enabled) noexcept { autoRestart_ = enabled; }
    [[nodiscard]] bool running() const noexcept { return process_.state() != QProcess::NotRunning; }

signals:
    void ready();
    void crashed();
    void restarted();
    void messageReceived(const SourceMessage& message);
    void requestTimedOut(const QString& requestId);
    void protocolError(const QString& message);

private:
    void processFrames();
    void clearPending();

    QString executablePath_;
    QProcess process_;
    QTimer restartTimer_;
    QByteArray readBuffer_;
    QHash<QString, QPointer<QTimer>> pending_;
    bool stopping_{false};
    bool handshakeComplete_{false};
    bool autoRestart_{true};
    int restartAttempts_{0};
    static constexpr qsizetype MaxPendingRequests = 256;
};

} // namespace listenfree::sourcehost
