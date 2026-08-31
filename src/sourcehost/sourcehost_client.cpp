#include "sourcehost/sourcehost_client.h"

#include <QDataStream>
#include <QIODevice>
#include <QTimer>
#include <QUuid>

#include <utility>

namespace listenfree::sourcehost {

SourceHostClient::SourceHostClient(QString executablePath, QObject* parent)
    : QObject(parent), executablePath_(std::move(executablePath)) {
    restartTimer_.setSingleShot(true);
    connect(&restartTimer_, &QTimer::timeout, this, [this] {
        if (!stopping_ && start()) emit restarted();
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError) {
        emit protocolError(process_.errorString());
    });
    connect(&process_, &QProcess::readyReadStandardError, this, [this] {
        const QByteArray error = process_.readAllStandardError().left(4096).trimmed();
        if (!error.isEmpty()) emit protocolError(QString::fromUtf8(error));
    });
    connect(&process_, &QProcess::readyRead, this, [this] {
        if (handshakeComplete_) processFrames();
    });
    connect(&process_, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        clearPending();
        if (status == QProcess::CrashExit || exitCode != 0) {
            readBuffer_.clear();
            emit crashed();
            if (autoRestart_ && !stopping_ && restartAttempts_ == 0) {
                ++restartAttempts_;
                restartTimer_.start(0);
            }
        } else {
            restartAttempts_ = 0;
        }
    });
}

SourceHostClient::~SourceHostClient() { stop(); }

bool SourceHostClient::start() {
    if (running() || executablePath_.isEmpty()) return false;
    stopping_ = false;
    handshakeComplete_ = false;
    readBuffer_.clear();
    process_.start(executablePath_);
    if (!process_.waitForStarted(1000) || !process_.waitForReadyRead(1000)) {
        stopping_ = true;
        process_.kill();
        process_.waitForFinished(1000);
        stopping_ = false;
        readBuffer_.clear();
        return false;
    }
    const QByteArray greeting = process_.readLine().trimmed();
    if (greeting != QByteArrayLiteral("listenfree-sourcehost-ready")) {
        emit protocolError(QStringLiteral("invalid-sourcehost-greeting"));
        stop();
        return false;
    }
    handshakeComplete_ = true;
    emit ready();
    return true;
}

void SourceHostClient::stop() noexcept {
    stopping_ = true;
    restartTimer_.stop();
    handshakeComplete_ = false;
    clearPending();
    readBuffer_.clear();
    if (!running()) {
        restartAttempts_ = 0;
        return;
    }
    SourceMessage shutdown;
    shutdown.type = MessageType::Shutdown;
    shutdown.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    process_.write(SourceProtocol::encode(shutdown));
    process_.waitForBytesWritten(200);
    if (!process_.waitForFinished(1000)) {
        process_.kill();
        process_.waitForFinished(1000);
    }
    stopping_ = false;
    restartAttempts_ = 0;
}

bool SourceHostClient::request(const SourceMessage& message, int timeoutMs) {
    if (!running() || message.requestId.isEmpty() || timeoutMs <= 0) return false;
    if (pending_.size() >= MaxPendingRequests) {
        emit protocolError(QStringLiteral("too-many-pending-requests"));
        return false;
    }
    if (const auto previous = pending_.take(message.requestId); previous) {
        previous->stop();
        previous->deleteLater();
    }
    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    const QString requestId = message.requestId;
    connect(timer, &QTimer::timeout, this, [this, requestId, timer] {
        pending_.remove(requestId);
        timer->deleteLater();
        emit requestTimedOut(requestId);
    });
    pending_.insert(requestId, timer);
    timer->start(timeoutMs);
    const QByteArray frame = SourceProtocol::encode(message);
    if (process_.write(frame) != frame.size() || !process_.waitForBytesWritten(200)) {
        pending_.remove(requestId);
        timer->stop();
        timer->deleteLater();
        return false;
    }
    return true;
}

bool SourceHostClient::loadPlugin(const std::filesystem::path& path) {
    if (!running()) return false;
    SourceMessage message;
    message.type = MessageType::LoadPlugin;
    message.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    message.payload.insert(QStringLiteral("path"), QString::fromStdString(path.string()));
    return request(message, 5000);
}

void SourceHostClient::cancel(const std::string& requestId) {
    if (!running()) return;
    const QString id = QString::fromStdString(requestId);
    if (const auto timer = pending_.take(id); timer) {
        timer->stop();
        timer->deleteLater();
    }
    SourceMessage message;
    message.type = MessageType::Cancel;
    message.requestId = id;
    message.payload.insert(QStringLiteral("requestId"), id);
    process_.write(SourceProtocol::encode(message));
    process_.waitForBytesWritten(200);
}

void SourceHostClient::clearPending() {
    for (const auto& timer : pending_) {
        if (timer) timer->stop();
        if (timer) timer->deleteLater();
    }
    pending_.clear();
}

void SourceHostClient::processFrames() {
    readBuffer_.append(process_.readAll());
    while (readBuffer_.size() >= 4) {
        QDataStream header(readBuffer_.left(4));
        header.setByteOrder(QDataStream::BigEndian);
        quint32 size = 0;
        header >> size;
        if (size == 0 || size > 1024U * 1024U) {
            emit protocolError(QStringLiteral("invalid-frame-size"));
            readBuffer_.clear();
            return;
        }
        const qsizetype frameSize = static_cast<qsizetype>(size) + 4;
        if (readBuffer_.size() < frameSize) return;
        const QByteArray frame = readBuffer_.left(frameSize);
        readBuffer_.remove(0, frameSize);
        SourceMessage message;
        QString error;
        if (!SourceProtocol::decode(frame, message, &error)) {
            emit protocolError(error);
            continue;
        }
        const bool completesRequest = message.type == MessageType::Result || message.type == MessageType::Error ||
                                      message.type == MessageType::HelloAck;
        if (completesRequest) {
            if (const auto timer = pending_.take(message.requestId); timer) {
                timer->stop();
                timer->deleteLater();
            }
        }
        emit messageReceived(message);
    }
}

} // namespace listenfree::sourcehost
