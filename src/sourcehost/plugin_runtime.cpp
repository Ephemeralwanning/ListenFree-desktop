#include "sourcehost/plugin_runtime.h"

#include <QFile>
#include <QFileInfo>
#include <QJSEngine>
#include <QJSValue>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
#include <QUrlQuery>

#include <zlib.h>

#include <memory>
#include <optional>
#include <utility>

namespace listenfree::sourcehost {

namespace {

constexpr qsizetype MaxPluginBytes = 1024 * 1024;
constexpr qsizetype MaxPendingPluginRequests = 256;
constexpr qsizetype MaxUrlBytes = 2048;
constexpr qsizetype MaxPendingNetworkRequests = 64;
constexpr qsizetype MaxNetworkResponseBytes = 8 * 1024 * 1024;

QString scriptError(const QJSValue& value) {
    QString result = value.toString();
    const QString stack = value.property(QStringLiteral("stack")).toString();
    if (!stack.isEmpty()) result += QStringLiteral(" | ") + stack;
    return result;
}

class PluginBridge final : public QObject {
    Q_OBJECT
public:
    Q_INVOKABLE void sendEvent(const QString& eventName, const QString& json) {
        emit eventSent(eventName, json);
    }

    Q_INVOKABLE void resolveRequest(const QString& requestId, const QString& json) {
        emit requestResolved(requestId, json);
    }

    Q_INVOKABLE void rejectRequest(const QString& requestId, const QString& message) {
        emit requestRejected(requestId, message);
    }

    Q_INVOKABLE void startRequest(const QString& requestId, const QString& url, const QString& optionsJson) {
        emit requestStarted(requestId, url, optionsJson);
    }

    Q_INVOKABLE void abortRequest(const QString& requestId) {
        emit requestAborted(requestId);
    }

signals:
    void eventSent(const QString& eventName, const QString& json);
    void requestResolved(const QString& requestId, const QString& json);
    void requestRejected(const QString& requestId, const QString& message);
    void requestStarted(const QString& requestId, const QString& url, const QString& optionsJson);
    void requestAborted(const QString& requestId);
};

const QString bootstrapScript = QStringLiteral(R"JS(
(function () {
    const root = this
    const handlers = Object.create(null)
    const eventNames = { request: 'request', inited: 'inited', updateAlert: 'updateAlert' }
    root.lx = {
        version: '2.0.0',
        env: 'desktop',
        EVENT_NAMES: eventNames,
        currentScriptInfo: {},
        on(name, handler) {
            if (!(name in eventNames) || typeof handler !== 'function') {
                return Promise.reject(new Error('unsupported event handler'))
            }
            handlers[name] = handler
            return Promise.resolve(true)
        },
        send(name, data) {
            if (!(name in eventNames)) return Promise.reject(new Error('unsupported event'))
            root.__lf_last_event = { name, data: data === undefined ? null : data }
            if (__lf_bridge && typeof __lf_bridge.sendEvent === 'function') {
                __lf_bridge.sendEvent(name, JSON.stringify(data === undefined ? null : data))
            }
            return Promise.resolve(true)
        },
        request(url, options, callback) {
            if (typeof url !== 'string' || typeof callback !== 'function') {
                throw new Error('invalid request arguments')
            }
            const requestId = 'http-' + (++root.__lf_request_sequence)
            root.__lf_request_callbacks[requestId] = callback
            try {
                __lf_bridge.startRequest(requestId, url, JSON.stringify(options || {}))
            } catch (error) {
                delete root.__lf_request_callbacks[requestId]
                throw error
            }
            return function () {
                delete root.__lf_request_callbacks[requestId]
                __lf_bridge.abortRequest(requestId)
            }
        }
    }
    root.__lf_request_sequence = 0
    root.__lf_request_callbacks = Object.create(null)
    root.__lf_request_complete = function (requestId, error, packetJson) {
        const callback = root.__lf_request_callbacks[requestId]
        delete root.__lf_request_callbacks[requestId]
        if (typeof callback !== 'function') return false
        if (error) {
            callback(new Error(error), null, null)
            return true
        }
        let packet
        try { packet = JSON.parse(packetJson) } catch (parseError) {
            callback(parseError, null, null)
            return false
        }
        callback(null, packet.response || null, packet.body)
        return true
    }
    root.__lf_dispatch = function (requestId, json) {
        let request
        try { request = JSON.parse(json) } catch (error) {
            __lf_bridge.rejectRequest(requestId, String(error))
            return false
        }
        const handler = handlers[eventNames.request]
        if (typeof handler !== 'function') {
            __lf_bridge.rejectRequest(requestId, 'request handler is not registered')
            return false
        }
        let result
        try { result = handler(request) } catch (error) {
            __lf_bridge.rejectRequest(requestId, String(error && error.message || error))
            return false
        }
        Promise.resolve(result).then(
            value => __lf_bridge.resolveRequest(requestId, JSON.stringify(value)),
            error => __lf_bridge.rejectRequest(requestId, String(error && error.message || error)))
        return true
    }
})()
)JS");

QJsonObject errorPayload(const QString& code, const QString& message) {
    return {{QStringLiteral("code"), code}, {QStringLiteral("message"), message}};
}

bool containsString(const QJsonArray& values, const QString& expected) {
    for (const auto& value : values) {
        if (value.toString() == expected) return true;
    }
    return false;
}

std::optional<QByteArray> decodePluginSource(const QByteArray& encoded, QString* error) {
    if (!encoded.startsWith("gz_")) return encoded;

    const QByteArray compressed = QByteArray::fromBase64(encoded.mid(3));
    if (compressed.isEmpty()) {
        if (error) *error = QStringLiteral("Compressed plugin payload is not valid base64.");
        return std::nullopt;
    }

    // The legacy desktop API stores zlib.deflate output after the `gz_` marker.
    // Decode into a bounded buffer so a malformed or adversarial script cannot
    // turn the SourceHost into an unbounded decompressor.
    constexpr uLong MaxOutput = static_cast<uLong>(MaxPluginBytes);
    uLong outputSize = 64U * 1024U;
    while (outputSize <= MaxOutput) {
        QByteArray output(static_cast<qsizetype>(outputSize), Qt::Uninitialized);
        uLong actualSize = outputSize;
        const int result = ::uncompress(reinterpret_cast<Bytef*>(output.data()), &actualSize,
                                        reinterpret_cast<const Bytef*>(compressed.constData()),
                                        static_cast<uLong>(compressed.size()));
        if (result == Z_OK) {
            output.resize(static_cast<qsizetype>(actualSize));
            return output;
        }
        if (result != Z_BUF_ERROR || outputSize == MaxOutput) break;
        outputSize = qMin(MaxOutput, outputSize * 2U);
    }
    if (error) *error = QStringLiteral("Compressed plugin payload is invalid or exceeds the 1 MiB limit.");
    return std::nullopt;
}

QJsonObject parseScriptMetadata(const QByteArray& source) {
    const QString text = QString::fromUtf8(source);
    const auto headerMatch = QRegularExpression(QStringLiteral(R"(^/\*[\s\S]*?\*/)"))
                                 .match(text);
    QJsonObject metadata;
    const QRegularExpression entry(QStringLiteral(R"(^\s?\*\s?@(\w+)\s+(.+)$)"));
    if (headerMatch.hasMatch()) {
        const QStringList lines = headerMatch.captured(0).split(QRegularExpression(QStringLiteral("\\r?\\n")));
        for (const auto& line : lines) {
            const auto match = entry.match(line);
            if (!match.hasMatch()) continue;
            const QString key = match.captured(1);
            const QString value = match.captured(2).trimmed();
            if (key == QStringLiteral("name")) metadata.insert(key, value.left(24));
            else if (key == QStringLiteral("description")) metadata.insert(key, value.left(36));
            else if (key == QStringLiteral("author")) metadata.insert(key, value.left(56));
            else if (key == QStringLiteral("homepage")) metadata.insert(key, value.left(1024));
            else if (key == QStringLiteral("version")) metadata.insert(key, value.left(36));
        }
    }
    for (const auto& key : {QStringLiteral("name"), QStringLiteral("description"),
                            QStringLiteral("author"), QStringLiteral("homepage"),
                            QStringLiteral("version")}) {
        if (!metadata.contains(key)) metadata.insert(key, QString());
    }
    metadata.insert(QStringLiteral("rawScript"), text);
    return metadata;
}

} // namespace

class PluginRuntime::Impl final {
public:
    explicit Impl(PluginRuntime& owner)
        : owner_(owner), network_(std::make_unique<QNetworkAccessManager>(&owner)) {}

    void handle(const SourceMessage& request) {
        switch (request.type) {
        case MessageType::LoadPlugin: load(request); break;
        case MessageType::UnloadPlugin: unload(request); break;
        case MessageType::Initialize: initialize(request); break;
        case MessageType::ResolveMusicUrl: resolve(request); break;
        case MessageType::ResolveLyric: resolve(request); break;
        case MessageType::ResolvePic: resolve(request); break;
        case MessageType::Cancel: cancel(request); break;
        default: respondError(request.requestId, QStringLiteral("plugin.unsupported-message"),
                               QStringLiteral("The plugin runtime does not handle this message."));
        }
    }

private:
    struct PendingRequest {
        QString source;
        QString action;
        QString type;
    };

    void startNetworkRequest(const QString& requestId, const QString& url, const QString& optionsJson) {
        if (!engine_ || pendingNetwork_.size() >= MaxPendingNetworkRequests) {
            completeNetworkRequest(requestId, QStringLiteral("request capacity exceeded"), {});
            return;
        }
        const QUrl parsedUrl(url);
        if (!parsedUrl.isValid() ||
            (parsedUrl.scheme() != QStringLiteral("http") && parsedUrl.scheme() != QStringLiteral("https")) ||
            parsedUrl.host().isEmpty() || url.size() > MaxUrlBytes) {
            completeNetworkRequest(requestId, QStringLiteral("invalid request URL"), {});
            return;
        }

        QJsonParseError parseError;
        const QJsonDocument optionsDocument = QJsonDocument::fromJson(optionsJson.toUtf8(), &parseError);
        const QJsonObject options = parseError.error == QJsonParseError::NoError && optionsDocument.isObject()
                                        ? optionsDocument.object()
                                        : QJsonObject{};
        QNetworkRequest networkRequest(parsedUrl);
        const QJsonObject headers = options.value(QStringLiteral("headers")).toObject();
        for (auto it = headers.begin(); it != headers.end(); ++it) {
            if (it.value().isString()) networkRequest.setRawHeader(it.key().toUtf8(), it.value().toString().toUtf8());
        }

        const int timeout = qBound(1, options.value(QStringLiteral("timeout")).toInt(60000), 60000);
        networkRequest.setTransferTimeout(timeout);
        QByteArray body;
        if (options.value(QStringLiteral("body")).isString()) {
            body = options.value(QStringLiteral("body")).toString().toUtf8();
        } else if (options.value(QStringLiteral("form")).isObject() || options.value(QStringLiteral("formData")).isObject()) {
            const QJsonObject form = options.value(QStringLiteral("form")).toObject().isEmpty()
                                         ? options.value(QStringLiteral("formData")).toObject()
                                         : options.value(QStringLiteral("form")).toObject();
            QUrlQuery query;
            for (auto it = form.begin(); it != form.end(); ++it) query.addQueryItem(it.key(), it.value().toString());
            body = query.toString(QUrl::FullyEncoded).toUtf8();
            if (!networkRequest.hasRawHeader("Content-Type")) {
                networkRequest.setHeader(QNetworkRequest::ContentTypeHeader,
                                         QStringLiteral("application/x-www-form-urlencoded"));
            }
        }

        const QByteArray method = options.value(QStringLiteral("method")).toString(QStringLiteral("get"))
                                      .toUpper()
                                      .toUtf8();
        QNetworkReply* reply = nullptr;
        if (method == "GET") reply = network_->get(networkRequest);
        else if (method == "POST") reply = network_->post(networkRequest, body);
        else if (method == "PUT") reply = network_->put(networkRequest, body);
        else if (method == "DELETE") reply = network_->deleteResource(networkRequest);
        else reply = network_->sendCustomRequest(networkRequest, method, body);
        if (!reply) {
            completeNetworkRequest(requestId, QStringLiteral("unable to create network request"), {});
            return;
        }
        pendingNetwork_.insert(requestId, reply);
        if (!activePluginRequest_.isEmpty()) pluginNetworkRequests_[activePluginRequest_].insert(requestId);
        connect(reply, &QNetworkReply::finished, &owner_, [this, requestId, reply] {
            finishNetworkRequest(requestId, reply);
        });
    }

    void abortNetworkRequest(const QString& requestId) {
        const auto it = pendingNetwork_.find(requestId);
        if (it == pendingNetwork_.end()) return;
        QNetworkReply* reply = it.value();
        pendingNetwork_.erase(it);
        for (auto parentIt = pluginNetworkRequests_.begin(); parentIt != pluginNetworkRequests_.end();) {
            parentIt.value().remove(requestId);
            if (parentIt.value().isEmpty()) parentIt = pluginNetworkRequests_.erase(parentIt);
            else ++parentIt;
        }
        if (reply) {
            QObject::disconnect(reply, nullptr, &owner_, nullptr);
            reply->abort();
            reply->deleteLater();
        }
    }

    void finishNetworkRequest(const QString& requestId, QNetworkReply* reply) {
        if (!pendingNetwork_.remove(requestId)) return;
        for (auto parentIt = pluginNetworkRequests_.begin(); parentIt != pluginNetworkRequests_.end();) {
            parentIt.value().remove(requestId);
            if (parentIt.value().isEmpty()) parentIt = pluginNetworkRequests_.erase(parentIt);
            else ++parentIt;
        }
        const QByteArray raw = reply->readAll();
        if (raw.size() > MaxNetworkResponseBytes) {
            completeNetworkRequest(requestId, QStringLiteral("response exceeds the 8 MiB limit"), {});
            reply->deleteLater();
            return;
        }
        if (reply->error() != QNetworkReply::NoError) {
            completeNetworkRequest(requestId, reply->errorString(), {});
            reply->deleteLater();
            return;
        }

        QJsonValue body = QString::fromUtf8(raw);
        QJsonParseError parseError;
        const QJsonDocument bodyDocument = QJsonDocument::fromJson(raw, &parseError);
        if (parseError.error == QJsonParseError::NoError) {
            if (bodyDocument.isObject()) body = bodyDocument.object();
            else if (bodyDocument.isArray()) body = bodyDocument.array();
        }
        QJsonObject response;
        response.insert(QStringLiteral("statusCode"), reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt());
        response.insert(QStringLiteral("statusMessage"), QString());
        QJsonObject responseHeaders;
        const auto headers = reply->rawHeaderPairs();
        for (const auto& header : headers) responseHeaders.insert(QString::fromUtf8(header.first), QString::fromUtf8(header.second));
        response.insert(QStringLiteral("headers"), responseHeaders);
        response.insert(QStringLiteral("bytes"), raw.size());
        response.insert(QStringLiteral("rawBase64"), QString::fromLatin1(raw.toBase64()));
        completeNetworkRequest(requestId, {}, QJsonObject{{QStringLiteral("response"), response},
                                                          {QStringLiteral("body"), body}});
        reply->deleteLater();
    }

    void completeNetworkRequest(const QString& requestId, const QString& error, const QJsonObject& packet) {
        if (!engine_) return;
        const QJSValue complete = engine_->globalObject().property(QStringLiteral("__lf_request_complete"));
        if (!complete.isCallable()) return;
        const QString packetJson = QString::fromUtf8(QJsonDocument(packet).toJson(QJsonDocument::Compact));
        complete.call({engine_->toScriptValue(requestId), engine_->toScriptValue(error),
                       engine_->toScriptValue(packetJson)});
    }

    void respond(const QString& requestId, MessageType type, QJsonObject payload) {
        SourceMessage response;
        response.type = type;
        response.requestId = requestId;
        response.payload = std::move(payload);
        emit owner_.responseReady(response);
    }

    void respondError(const QString& requestId, const QString& code, const QString& message) {
        respond(requestId, MessageType::Error, errorPayload(code, message));
    }

    void reset() {
        pending_.clear();
        for (auto reply : std::as_const(pendingNetwork_)) {
            if (!reply) continue;
            QObject::disconnect(reply, nullptr, &owner_, nullptr);
            reply->abort();
            reply->deleteLater();
        }
        pendingNetwork_.clear();
        pluginNetworkRequests_.clear();
        activePluginRequest_.clear();
        initialized_ = false;
        initPayload_ = {};
        dispatch_ = {};
        bridge_.reset();
        engine_.reset();
    }

    void load(const SourceMessage& request) {
        const QString path = request.payload.value(QStringLiteral("path")).toString();
        const QFileInfo info(path);
        if (path.isEmpty() || !info.exists() || !info.isFile()) {
            respondError(request.requestId, QStringLiteral("plugin.file-not-found"),
                         QStringLiteral("Plugin file does not exist."));
            return;
        }
        if (info.size() > MaxPluginBytes) {
            respondError(request.requestId, QStringLiteral("plugin.file-too-large"),
                         QStringLiteral("Plugin file exceeds the 1 MiB limit."));
            return;
        }
        QFile file(info.absoluteFilePath());
        if (!file.open(QIODevice::ReadOnly)) {
            respondError(request.requestId, QStringLiteral("plugin.file-open-failed"), file.errorString());
            return;
        }
        const QByteArray encodedSource = file.readAll();
        QString sourceError;
        const auto source = decodePluginSource(encodedSource, &sourceError);
        if (!source) {
            respondError(request.requestId, QStringLiteral("plugin.decode-failed"), sourceError);
            return;
        }

        reset();
        engine_ = std::make_unique<QJSEngine>();
        bridge_ = std::make_unique<PluginBridge>();
        QObject::connect(bridge_.get(), &PluginBridge::eventSent, [this](const QString& name, const QString& json) {
            handleEvent(name, json);
        });
        QObject::connect(bridge_.get(), &PluginBridge::requestResolved,
                         [this](const QString& id, const QString& json) { handleResolved(id, json); });
        QObject::connect(bridge_.get(), &PluginBridge::requestRejected,
                         [this](const QString& id, const QString& message) { handleRejected(id, message); });
        QObject::connect(bridge_.get(), &PluginBridge::requestStarted,
                         [this](const QString& id, const QString& url, const QString& options) {
                             startNetworkRequest(id, url, options);
                         });
        QObject::connect(bridge_.get(), &PluginBridge::requestAborted,
                         [this](const QString& id) { abortNetworkRequest(id); });

        engine_->globalObject().setProperty(QStringLiteral("__lf_bridge"), engine_->newQObject(bridge_.get()));
        const QJSValue bootstrap = engine_->evaluate(bootstrapScript, info.absoluteFilePath(), 1);
        if (engine_->hasError()) {
            const QJSValue error = engine_->catchError();
            reset();
            respondError(request.requestId, QStringLiteral("plugin.bootstrap-failed"), scriptError(error));
            return;
        }
        QJSValue lx = engine_->globalObject().property(QStringLiteral("lx"));
        lx.setProperty(QStringLiteral("currentScriptInfo"), engine_->toScriptValue(parseScriptMetadata(*source).toVariantMap()));
        const QJSValue result = engine_->evaluate(QString::fromUtf8(*source), info.absoluteFilePath(), 1);
        if (engine_->hasError()) {
            const QJSValue error = engine_->catchError();
            reset();
            respondError(request.requestId, QStringLiteral("plugin.script-failed"), scriptError(error));
            return;
        }
        const QJSValue lastEvent = engine_->globalObject().property(QStringLiteral("__lf_last_event"));
        if (!initialized_ && lastEvent.isObject()) {
            const QString eventName = lastEvent.property(QStringLiteral("name")).toString();
            const QVariant eventData = lastEvent.property(QStringLiteral("data")).toVariant();
            const QJsonValue jsonValue = QJsonValue::fromVariant(eventData);
            const QJsonDocument eventDocument = jsonValue.isObject()
                                                     ? QJsonDocument(jsonValue.toObject())
                                                     : QJsonDocument(QJsonArray{jsonValue});
            handleEvent(eventName, jsonValue.isObject()
                                      ? QString::fromUtf8(eventDocument.toJson(QJsonDocument::Compact))
                                      : QString{});
        }
        dispatch_ = engine_->globalObject().property(QStringLiteral("__lf_dispatch"));
        if (!initialized_ || !initPayload_.value(QStringLiteral("status")).toBool()) {
            reset();
            respondError(request.requestId, QStringLiteral("plugin.init-missing"),
                         QStringLiteral("Plugin did not report a successful inited event."));
            return;
        }
        respond(request.requestId, MessageType::Result,
                {{QStringLiteral("ok"), true}, {QStringLiteral("sources"), initPayload_.value(QStringLiteral("sources"))}});
    }

    void unload(const SourceMessage& request) {
        for (const auto& id : pending_.keys()) {
            respondError(id, QStringLiteral("plugin.unloaded"), QStringLiteral("Plugin was unloaded."));
        }
        reset();
        respond(request.requestId, MessageType::Result, {{QStringLiteral("ok"), true}});
    }

    void initialize(const SourceMessage& request) {
        if (!engine_ || !initialized_) {
            respondError(request.requestId, QStringLiteral("plugin.not-loaded"),
                         QStringLiteral("No initialized plugin is loaded."));
            return;
        }
        respond(request.requestId, MessageType::Result, initPayload_);
    }

    void cancel(const SourceMessage& request) {
        const QString targetId = request.payload.value(QStringLiteral("requestId")).toString().isEmpty()
                                     ? request.requestId
                                     : request.payload.value(QStringLiteral("requestId")).toString();
        pending_.remove(targetId);
        const auto networkIds = pluginNetworkRequests_.take(targetId);
        for (const auto& networkId : networkIds) abortNetworkRequest(networkId);
        respondError(targetId, QStringLiteral("plugin.cancelled"), QStringLiteral("Plugin request was cancelled."));
        respond(request.requestId, MessageType::Result, {{QStringLiteral("ok"), true}});
    }

    void resolve(const SourceMessage& request) {
        if (!engine_ || !initialized_) {
            respondError(request.requestId, QStringLiteral("plugin.not-loaded"),
                         QStringLiteral("No initialized plugin is loaded."));
            return;
        }
        if (pending_.size() >= MaxPendingPluginRequests) {
            respondError(request.requestId, QStringLiteral("plugin.too-many-requests"),
                         QStringLiteral("Too many plugin requests are pending."));
            return;
        }
        const QString source = request.payload.value(QStringLiteral("source")).toString();
        const QString type = request.payload.value(QStringLiteral("type")).toString();
        const QString action = request.type == MessageType::ResolveLyric
                                   ? QStringLiteral("lyric")
                                   : request.type == MessageType::ResolvePic ? QStringLiteral("pic")
                                                                              : QStringLiteral("musicUrl");
        const QJsonObject musicInfo = request.payload.value(QStringLiteral("musicInfo")).toObject();
        const QJsonObject sourceInfo = initPayload_.value(QStringLiteral("sources")).toObject()
                                           .value(source).toObject();
        if (sourceInfo.isEmpty() || !containsString(sourceInfo.value(QStringLiteral("actions")).toArray(), action) ||
            (action == QStringLiteral("musicUrl") &&
             !containsString(sourceInfo.value(QStringLiteral("qualitys")).toArray(), type)) ||
            musicInfo.isEmpty()) {
            respondError(request.requestId, QStringLiteral("plugin.invalid-request"),
                         QStringLiteral("Source, quality or musicInfo is not supported."));
            return;
        }
        pending_.insert(request.requestId, {source, action, type});
        QJsonObject info;
        info.insert(QStringLiteral("type"), type);
        info.insert(QStringLiteral("musicInfo"), musicInfo);
        QJsonObject event;
        event.insert(QStringLiteral("source"), source);
        event.insert(QStringLiteral("action"), action);
        event.insert(QStringLiteral("info"), info);
        const QString previousActiveRequest = activePluginRequest_;
        activePluginRequest_ = request.requestId;
        const QJSValue accepted = dispatch_.call({engine_->toScriptValue(request.requestId),
                                                  engine_->toScriptValue(QString::fromUtf8(
                                                      QJsonDocument(event).toJson(QJsonDocument::Compact)))});
        activePluginRequest_ = previousActiveRequest;
        if (accepted.isError() || !accepted.toBool()) handleRejected(request.requestId, accepted.toString());
    }

    void handleEvent(const QString& name, const QString& json) {
        QJsonParseError parseError;
        const QJsonDocument document = QJsonDocument::fromJson(json.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError || !document.isObject()) return;
        if (name == QStringLiteral("inited") && !initialized_) {
            initialized_ = true;
            initPayload_ = document.object();
        }
    }

    void handleResolved(const QString& requestId, const QString& json) {
        const auto pending = pending_.take(requestId);
        if (pending.source.isEmpty()) return;
        QJsonParseError parseError;
        const QJsonValue value = QJsonValue::fromJson(json.toUtf8(), &parseError);
        if (parseError.error != QJsonParseError::NoError) {
            respondError(requestId, QStringLiteral("plugin.invalid-response"),
                         QStringLiteral("Plugin returned invalid JSON."));
            return;
        }
        if (pending.action == QStringLiteral("lyric")) {
            if (!value.isObject()) {
                respondError(requestId, QStringLiteral("plugin.invalid-lyric"),
                             QStringLiteral("Plugin returned invalid lyric data."));
                return;
            }
            const QJsonObject lyric = value.toObject();
            const QString lyricText = lyric.value(QStringLiteral("lyric")).toString();
            if (!lyric.value(QStringLiteral("lyric")).isString() || lyricText.size() > 51200) {
                respondError(requestId, QStringLiteral("plugin.invalid-lyric"),
                             QStringLiteral("Plugin returned invalid lyric data."));
                return;
            }
            auto boundedOptional = [](const QJsonValue& candidate, int maxLength) {
                return candidate.isString() && candidate.toString().size() < maxLength
                           ? candidate.toString()
                           : QString();
            };
            respond(requestId, MessageType::Result,
                    {{QStringLiteral("source"), pending.source},
                     {QStringLiteral("action"), pending.action},
                     {QStringLiteral("data"), QJsonObject{
                                                   {QStringLiteral("lyric"), lyricText},
                                                   {QStringLiteral("tlyric"), boundedOptional(lyric.value(QStringLiteral("tlyric")), 5120)},
                                                   {QStringLiteral("rlyric"), boundedOptional(lyric.value(QStringLiteral("rlyric")), 5120)},
                                                   {QStringLiteral("lxlyric"), boundedOptional(lyric.value(QStringLiteral("lxlyric")), 8192)}}}});
            return;
        }
        const QString url = value.isString() ? value.toString() : QString{};
        const QUrl parsed(url);
        if (url.isEmpty() || url.size() > MaxUrlBytes ||
            (parsed.scheme() != QStringLiteral("http") && parsed.scheme() != QStringLiteral("https")) ||
            parsed.host().isEmpty()) {
            respondError(requestId, QStringLiteral("plugin.invalid-url"),
                         QStringLiteral("Plugin returned an invalid media URL."));
            return;
        }
        const QJsonValue data = pending.action == QStringLiteral("pic")
                                    ? QJsonValue(url)
                                    : QJsonValue(QJsonObject{{QStringLiteral("type"), pending.type},
                                                             {QStringLiteral("url"), url}});
        respond(requestId, MessageType::Result,
                {{QStringLiteral("source"), pending.source},
                 {QStringLiteral("action"), pending.action},
                 {QStringLiteral("data"), data}});
    }

    void handleRejected(const QString& requestId, const QString& message) {
        if (!pending_.remove(requestId)) return;
        respondError(requestId, QStringLiteral("plugin.request-failed"), message);
    }

    PluginRuntime& owner_;
    std::unique_ptr<QNetworkAccessManager> network_;
    std::unique_ptr<QJSEngine> engine_;
    std::unique_ptr<PluginBridge> bridge_;
    QJSValue dispatch_;
    QJsonObject initPayload_;
    QHash<QString, PendingRequest> pending_;
    QHash<QString, QNetworkReply*> pendingNetwork_;
    QHash<QString, QSet<QString>> pluginNetworkRequests_;
    QString activePluginRequest_;
    bool initialized_{false};
};

PluginRuntime::PluginRuntime(QObject* parent) : QObject(parent), impl_(std::make_unique<Impl>(*this)) {
    // The dispatch function is resolved after each load; keeping this signal
    // connection local ensures all script callbacks stay on the SourceHost
    // event-loop thread.
}

PluginRuntime::~PluginRuntime() = default;

void PluginRuntime::handle(const SourceMessage& request) {
    impl_->handle(request);
}

} // namespace listenfree::sourcehost

#include "plugin_runtime.moc"
