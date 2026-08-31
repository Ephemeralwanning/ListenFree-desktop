#include "sourcehost/sourcehost_client.h"

#include <QDataStream>
#include <QIODevice>
#include <QTimer>
#include <QUuid>

#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace listenfree::sourcehost {

#ifdef Q_OS_WIN
void SourceHostClient::JobHandleDeleter::operator()(void* handle) const noexcept {
    if (handle != nullptr) CloseHandle(static_cast<HANDLE>(handle));
}

bool SourceHostClient::createJobObject() {
    if (jobHandle_) return true;
    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) {
        emit protocolError(QStringLiteral("sourcehost-job-create-failed:%1").arg(GetLastError()));
        return false;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        const auto error = GetLastError();
        CloseHandle(job);
        emit protocolError(QStringLiteral("sourcehost-job-configure-failed:%1").arg(error));
        return false;
    }
    jobHandle_.reset(job);
    return true;
}

bool SourceHostClient::assignProcessToJob() {
    if (!jobHandle_) return false;
    const auto processId = static_cast<DWORD>(process_.processId());
    if (processId == 0) return false;
    HANDLE processHandle = OpenProcess(PROCESS_SET_QUOTA | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
                                       FALSE, processId);
    if (processHandle == nullptr) {
        emit protocolError(QStringLiteral("sourcehost-process-open-failed:%1").arg(GetLastError()));
        return false;
    }
    const BOOL assigned = AssignProcessToJobObject(static_cast<HANDLE>(jobHandle_.get()), processHandle);
    const auto error = assigned ? ERROR_SUCCESS : GetLastError();
    CloseHandle(processHandle);
    if (!assigned) {
        emit protocolError(QStringLiteral("sourcehost-job-assign-failed:%1").arg(error));
        return false;
    }
    return true;
}

void SourceHostClient::terminateProcessTree() noexcept {
    if (jobHandle_) TerminateJobObject(static_cast<HANDLE>(jobHandle_.get()), 1);
}

void SourceHostClient::closeJobObject() noexcept {
    terminateProcessTree();
    jobHandle_.reset();
}
#endif

#ifndef Q_OS_WIN
void SourceHostClient::terminateProcessTree() noexcept {}
void SourceHostClient::closeJobObject() noexcept {}
#endif

SourceHostClient::SourceHostClient(QString executablePath, QObject* parent)
    : QObject(parent), executablePath_(std::move(executablePath)) {
    restartTimer_.setSingleShot(true);
    handshakeTimer_.setSingleShot(true);
    stopTimer_.setSingleShot(true);
    connect(&restartTimer_, &QTimer::timeout, this, [this] {
        if (!stopping_ && start()) restartInProgress_ = true;
    });
    connect(&handshakeTimer_, &QTimer::timeout, this, [this] {
        failHandshake(QStringLiteral("sourcehost-handshake-timeout"));
    });
    connect(&stopTimer_, &QTimer::timeout, this, [this] {
        terminateProcessTree();
        if (running()) process_.kill();
    });
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        emit protocolError(process_.errorString());
        if (error == QProcess::WriteError) {
            finishAll(RequestTerminal::WriteFailed);
        }
        if (error == QProcess::FailedToStart) {
            handshakeTimer_.stop();
            closeJobObject();
            transitionTo(HostState::Stopped);
        }
    });
    connect(&process_, &QProcess::readyReadStandardError, this, [this] {
        const QByteArray error = process_.readAllStandardError().left(4096).trimmed();
        if (!error.isEmpty()) emit protocolError(QString::fromUtf8(error));
    });
    connect(&process_, &QProcess::started, this, [this] {
        if (stopping_) {
            terminateProcessTree();
            process_.kill();
            return;
        }
        if (!assignProcessToJob()) {
            stopping_ = true;
            transitionTo(HostState::Stopping);
            process_.kill();
            return;
        }
        SourceMessage hello;
        hello.type = MessageType::Hello;
        hello.requestId = handshakeRequestId_;
        if (process_.write(SourceProtocol::encode(hello)) < 0) {
            failHandshake(QStringLiteral("sourcehost-hello-write-failed"));
        }
    });
    connect(&process_, &QProcess::readyRead, this, &SourceHostClient::handleStandardOutput);
    connect(&process_, &QProcess::finished, this, [this](int exitCode, QProcess::ExitStatus status) {
        handshakeTimer_.stop();
        stopTimer_.stop();
        closeJobObject();
        handshakeComplete_ = false;
        const bool failed = status == QProcess::CrashExit || exitCode != 0;
        if (failed && !stopping_) {
            finishAll(RequestTerminal::HostCrashed);
            readBuffer_.clear();
            emit crashed();
            if (autoRestart_ && !stopping_ && restartAttempts_ == 0) {
                ++restartAttempts_;
                transitionTo(HostState::RestartWaiting);
                restartTimer_.start(0);
            } else {
                transitionTo(HostState::Stopped);
            }
        } else {
            if (!stopping_) finishAll(RequestTerminal::HostStopped);
            restartAttempts_ = 0;
            transitionTo(HostState::Stopped);
        }
        if (stopping_) {
            stopping_ = false;
            restartAttempts_ = 0;
            restartInProgress_ = false;
            transitionTo(HostState::Stopped);
        }
    });
}

SourceHostClient::~SourceHostClient() {
    stop();
    if (running()) {
        terminateProcessTree();
        process_.kill();
    }
    closeJobObject();
}

bool SourceHostClient::start() {
    if (running() || executablePath_.isEmpty()) return false;
#ifdef Q_OS_WIN
    if (!createJobObject()) return false;
#endif
    stopTimer_.stop();
    stopping_ = false;
    handshakeComplete_ = false;
    handshakeRequestId_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    readBuffer_.clear();
    process_.start(executablePath_);
    handshakeTimer_.start(1000);
    transitionTo(HostState::Starting);
    return true;
}

void SourceHostClient::stop() noexcept {
    stopping_ = true;
    restartTimer_.stop();
    handshakeTimer_.stop();
    handshakeComplete_ = false;
    finishAll(RequestTerminal::HostStopped);
    readBuffer_.clear();
    if (!running()) {
        restartAttempts_ = 0;
        restartInProgress_ = false;
        transitionTo(HostState::Stopped);
        return;
    }
    SourceMessage shutdown;
    shutdown.type = MessageType::Shutdown;
    shutdown.requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    process_.write(SourceProtocol::encode(shutdown));
    stopTimer_.start(1000);
    transitionTo(HostState::Stopping);
}

bool SourceHostClient::request(const SourceMessage& message, int timeoutMs) {
    if (!running() || !handshakeComplete_ || message.requestId.isEmpty() || timeoutMs <= 0) return false;
    if (pending_.size() >= MaxPendingRequests) {
        emit protocolError(QStringLiteral("too-many-pending-requests"));
        return false;
    }
    if (pending_.contains(message.requestId)) {
        emit protocolError(QStringLiteral("duplicate-request-id"));
        return false;
    }
    auto* timer = new QTimer(this);
    timer->setSingleShot(true);
    const QString requestId = message.requestId;
    connect(timer, &QTimer::timeout, this, [this, requestId] {
        if (!finishRequest(requestId, RequestTerminal::TimedOut)) return;
        emit requestTimedOut(requestId);
    });
    pending_.insert(requestId, timer);
    timer->start(timeoutMs);
    const QByteArray frame = SourceProtocol::encode(message);
    if (frame.size() <= 4 || frame.size() > (1024 * 1024 + 4)) {
        emit protocolError(QStringLiteral("outgoing-frame-too-large"));
        finishRequest(requestId, RequestTerminal::WriteFailed);
        return false;
    }
    if (process_.write(frame) != frame.size()) {
        finishRequest(requestId, RequestTerminal::WriteFailed);
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
    if (!running() || !handshakeComplete_) return;
    const QString id = QString::fromStdString(requestId);
    if (!finishRequest(id, RequestTerminal::Cancelled)) return;
    SourceMessage message;
    message.type = MessageType::Cancel;
    message.requestId = id;
    message.payload.insert(QStringLiteral("requestId"), id);
    process_.write(SourceProtocol::encode(message));
}

void SourceHostClient::handleStandardOutput() {
    processFrames();
}

void SourceHostClient::transitionTo(HostState state) {
    if (state_ == state) return;
    state_ = state;
    emit stateChanged(state_);
}

void SourceHostClient::failHandshake(const QString& reason) {
    if (handshakeComplete_ || process_.state() == QProcess::NotRunning) return;
    handshakeTimer_.stop();
    emit protocolError(reason);
    stopping_ = true;
    restartInProgress_ = false;
    transitionTo(HostState::Stopping);
    process_.kill();
}

bool SourceHostClient::finishRequest(const QString& requestId, RequestTerminal terminal) {
    const auto timer = pending_.take(requestId);
    if (!timer) return false;
    timer->stop();
    timer->deleteLater();
    emit requestFinished(requestId, terminal);
    return true;
}

void SourceHostClient::finishAll(RequestTerminal terminal) {
    const auto requestIds = pending_.keys();
    for (const auto& requestId : requestIds) finishRequest(requestId, terminal);
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
        if (!handshakeComplete_) {
            if (message.type != MessageType::HelloAck || message.requestId != handshakeRequestId_) {
                failHandshake(QStringLiteral("invalid-sourcehost-handshake"));
                return;
            }
            handshakeTimer_.stop();
            handshakeComplete_ = true;
            transitionTo(HostState::Ready);
            emit ready();
            if (restartInProgress_) {
                restartInProgress_ = false;
                emit restarted();
            }
            continue;
        }
        if (message.type == MessageType::Result) {
            finishRequest(message.requestId, RequestTerminal::Succeeded);
        } else if (message.type == MessageType::Error) {
            finishRequest(message.requestId, RequestTerminal::RemoteError);
        }
        emit messageReceived(message);
    }
}

} // namespace listenfree::sourcehost
