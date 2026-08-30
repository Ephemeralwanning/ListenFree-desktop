#include "sourcehost/sourcehost_client.h"

#include <QTextStream>

namespace listenfree::sourcehost {

SourceHostClient::SourceHostClient(QString executablePath, QObject* parent)
    : QObject(parent), executablePath_(std::move(executablePath)) {
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        emit protocolError(process_.errorString());
    });
    connect(&process_, &QProcess::finished, this, [this](int, QProcess::ExitStatus status) {
        if (status == QProcess::CrashExit) emit crashed();
    });
}

SourceHostClient::~SourceHostClient() { stop(); }

bool SourceHostClient::start() {
    if (running() || executablePath_.isEmpty()) return false;
    process_.start(executablePath_);
    if (!process_.waitForStarted(1000) || !process_.waitForReadyRead(1000)) return false;
    const QByteArray greeting = process_.readLine().trimmed();
    if (greeting != QByteArrayLiteral("listenfree-sourcehost-ready")) {
        emit protocolError(QStringLiteral("invalid-sourcehost-greeting"));
        stop();
        return false;
    }
    emit ready();
    return true;
}

void SourceHostClient::stop() noexcept {
    if (!running()) return;
    process_.write("shutdown\n");
    process_.waitForBytesWritten(200);
    if (!process_.waitForFinished(1000)) {
        process_.kill();
        process_.waitForFinished(1000);
    }
}

bool SourceHostClient::loadPlugin(const std::filesystem::path& path) {
    if (!running()) return false;
    process_.write("loadPlugin:");
    process_.write(QString::fromStdString(path.string()).toUtf8());
    process_.write("\n");
    return process_.waitForBytesWritten(200);
}

void SourceHostClient::cancel(const std::string& requestId) {
    if (!running()) return;
    process_.write("cancel:");
    process_.write(QString::fromStdString(requestId).toUtf8());
    process_.write("\n");
    process_.waitForBytesWritten(200);
}

} // namespace listenfree::sourcehost
