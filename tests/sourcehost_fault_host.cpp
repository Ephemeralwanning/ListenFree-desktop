#include "sourcehost/source_protocol.h"

#include <QCoreApplication>
#include <QFile>
#include <QProcess>
#include <QThread>
#include <QMetaObject>

#include <array>
#include <cstdlib>
#include <functional>
#include <iostream>

#ifdef Q_OS_WIN
#include <windows.h>
#include <io.h>
#endif

using listenfree::sourcehost::MessageType;
using listenfree::sourcehost::SourceMessage;
using listenfree::sourcehost::SourceProtocol;

namespace {

QString mode() { return qEnvironmentVariable("LISTENFREE_FAULT_MODE", QStringLiteral("normal")); }

class StdinReader final : public QThread {
public:
    using Handler = std::function<void(QByteArray)>;
    explicit StdinReader(Handler handler) : handler_(std::move(handler)) {}

protected:
    void run() override {
        bool shutdownReceived = false;
        while (!isInterruptionRequested()) {
            std::array<char, 4> header{};
            std::cin.read(header.data(), static_cast<std::streamsize>(header.size()));
            if (std::cin.gcount() != static_cast<std::streamsize>(header.size())) break;
            const auto* bytes = reinterpret_cast<const unsigned char*>(header.data());
            const quint32 bodySize = (static_cast<quint32>(bytes[0]) << 24U) |
                                     (static_cast<quint32>(bytes[1]) << 16U) |
                                     (static_cast<quint32>(bytes[2]) << 8U) |
                                     static_cast<quint32>(bytes[3]);
            if (bodySize == 0 || bodySize > 1024U * 1024U) break;
            QByteArray frame(header.data(), 4);
            QByteArray body(static_cast<qsizetype>(bodySize), Qt::Uninitialized);
            std::cin.read(body.data(), static_cast<std::streamsize>(body.size()));
            if (std::cin.gcount() != static_cast<std::streamsize>(body.size())) break;
            frame.append(body);
            const auto handler = handler_;
            QMetaObject::invokeMethod(QCoreApplication::instance(), [handler, frame] { handler(frame); },
                                      Qt::BlockingQueuedConnection);
            SourceMessage message;
            if (SourceProtocol::decode(frame, message) && message.type == MessageType::Shutdown) {
                shutdownReceived = true;
                break;
            }
        }
        if (!shutdownReceived && !isInterruptionRequested() && mode() != QStringLiteral("writefail")) {
            QMetaObject::invokeMethod(QCoreApplication::instance(), [] { QCoreApplication::exit(3); },
                                      Qt::QueuedConnection);
        }
    }

private:
    Handler handler_;
};

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    const QString faultMode = mode();
    QProcess* spawned = nullptr;

    StdinReader reader([&app, faultMode, &spawned](QByteArray frame) {
        SourceMessage request;
        QString error;
        if (!SourceProtocol::decode(frame, request, &error)) return;

        if (request.type == MessageType::Shutdown) {
            if (faultMode != QStringLiteral("ignore-stop")) app.quit();
            return;
        }
        if (request.type == MessageType::Hello) {
            SourceMessage ack;
            ack.type = MessageType::HelloAck;
            ack.requestId = request.requestId;
            const QByteArray encoded = SourceProtocol::encode(ack);
            std::cout.write(encoded.constData(), static_cast<std::streamsize>(encoded.size()));
            std::cout.flush();
            if (faultMode == QStringLiteral("writefail")) {
#ifdef Q_OS_WIN
                _close(_fileno(stdin));
#endif
            }
            return;
        }

        if (faultMode == QStringLiteral("crash")) std::abort();
        if (faultMode == QStringLiteral("hang") || faultMode == QStringLiteral("ignore-stop")) {
            if (request.payload.value(QStringLiteral("spawnTree")).toBool() && spawned == nullptr) {
                spawned = new QProcess(&app);
                const QString pidFile = qEnvironmentVariable("LISTENFREE_CHILD_PID_FILE");
                QObject::connect(spawned, &QProcess::started, &app, [spawned, pidFile] {
                    if (pidFile.isEmpty()) return;
                    QFile file(pidFile);
                    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                        file.write(QByteArray::number(spawned->processId()));
                        file.flush();
                    }
                });
                spawned->start(QStringLiteral("cmd.exe"),
                               {QStringLiteral("/c"), QStringLiteral("ping 127.0.0.1 -n 60 >nul")});
            }
            return;
        }

        SourceMessage response;
        response.type = MessageType::Result;
        response.requestId = request.requestId;
        response.payload.insert(QStringLiteral("ok"), true);
        const QByteArray encoded = SourceProtocol::encode(response);
        std::cout.write(encoded.constData(), static_cast<std::streamsize>(encoded.size()));
        std::cout.flush();
    });
    reader.start();
    const int code = app.exec();
    reader.requestInterruption();
    reader.wait();
    return code;
}
