#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace listenfree::sourcehost {

enum class MessageType {
    Hello,
    HelloAck,
    LoadPlugin,
    UnloadPlugin,
    Initialize,
    ResolveMusicUrl,
    ResolveLyric,
    ResolvePic,
    Search,
    GetPlaylist,
    GetChart,
    Cancel,
    Result,
    Error,
    Log,
    Shutdown
};

struct SourceMessage {
    int protocolVersion{1};
    MessageType type{MessageType::Error};
    QString requestId;
    QJsonObject payload;
};

class SourceProtocol final {
public:
    static QByteArray encode(const SourceMessage& message);
    static bool decode(const QByteArray& frame, SourceMessage& message, QString* error = nullptr);
    static QString typeName(MessageType type);
    static bool typeFromName(const QString& name, MessageType& type);
};

} // namespace listenfree::sourcehost
