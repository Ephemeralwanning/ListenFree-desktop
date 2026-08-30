#include "sourcehost/source_protocol.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QThread>

#include <array>
#include <functional>
#include <iostream>
#include <utility>

namespace {

class StdinReader final : public QThread {
public:
    using Handler = std::function<void(QByteArray)>;

    explicit StdinReader(Handler handler) : handler_(std::move(handler)) {}

protected:
    void run() override {
        while (!isInterruptionRequested()) {
            std::array<char, 4> header{};
            std::cin.read(header.data(), static_cast<std::streamsize>(header.size()));
            if (std::cin.gcount() != static_cast<std::streamsize>(header.size())) break;
            QByteArray frame(header.data(), static_cast<qsizetype>(header.size()));
            const auto* bytes = reinterpret_cast<const unsigned char*>(header.data());
            const quint32 bodySize = (static_cast<quint32>(bytes[0]) << 24U) |
                                     (static_cast<quint32>(bytes[1]) << 16U) |
                                     (static_cast<quint32>(bytes[2]) << 8U) |
                                     static_cast<quint32>(bytes[3]);
            if (bodySize == 0 || bodySize > 1024U * 1024U) break;
            QByteArray body(static_cast<qsizetype>(bodySize), Qt::Uninitialized);
            std::cin.read(body.data(), static_cast<std::streamsize>(body.size()));
            if (std::cin.gcount() != static_cast<std::streamsize>(body.size())) break;
            frame.append(body);
            const auto handler = handler_;
            QMetaObject::invokeMethod(QCoreApplication::instance(), [handler, frame] { handler(frame); },
                                      Qt::QueuedConnection);

            listenfree::sourcehost::SourceMessage message;
            if (listenfree::sourcehost::SourceProtocol::decode(frame, message) &&
                message.type == listenfree::sourcehost::MessageType::Shutdown) {
                break;
            }
        }
    }

private:
    Handler handler_;
};

} // namespace

int main(int argc, char* argv[]) {
    QCoreApplication app(argc, argv);
    std::cout << "listenfree-sourcehost-ready\n" << std::flush;

    StdinReader reader([&app](QByteArray frame) {
        listenfree::sourcehost::SourceMessage request;
        QString error;
        if (!listenfree::sourcehost::SourceProtocol::decode(frame, request, &error)) return;
        if (request.type == listenfree::sourcehost::MessageType::Shutdown) {
            app.quit();
            return;
        }
        if (request.payload.value(QStringLiteral("noReply")).toBool()) return;
        listenfree::sourcehost::SourceMessage response;
        response.type = request.type == listenfree::sourcehost::MessageType::Hello
                            ? listenfree::sourcehost::MessageType::HelloAck
                            : listenfree::sourcehost::MessageType::Result;
        response.requestId = request.requestId;
        response.payload.insert(QStringLiteral("ok"), true);
        response.payload.insert(QStringLiteral("messageType"),
                                listenfree::sourcehost::SourceProtocol::typeName(request.type));
        const QByteArray encoded = listenfree::sourcehost::SourceProtocol::encode(response);
        std::cout.write(encoded.constData(), static_cast<std::streamsize>(encoded.size()));
        std::cout.flush();
    });
    reader.start();
    const int exitCode = app.exec();
    reader.requestInterruption();
    reader.wait();
    return exitCode;
}
