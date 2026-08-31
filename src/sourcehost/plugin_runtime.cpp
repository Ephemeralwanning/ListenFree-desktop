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
#include <QSet>
#include <QUrl>

#include <utility>

namespace listenfree::sourcehost {

namespace {

constexpr qsizetype MaxPluginBytes = 1024 * 1024;
constexpr qsizetype MaxPendingPluginRequests = 256;
constexpr qsizetype MaxUrlBytes = 2048;

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

signals:
    void eventSent(const QString& eventName, const QString& json);
    void requestResolved(const QString& requestId, const QString& json);
    void requestRejected(const QString& requestId, const QString& message);
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
        }
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

} // namespace

class PluginRuntime::Impl final {
public:
    explicit Impl(PluginRuntime& owner) : owner_(owner) {}

    void handle(const SourceMessage& request) {
        switch (request.type) {
        case MessageType::LoadPlugin: load(request); break;
        case MessageType::UnloadPlugin: unload(request); break;
        case MessageType::Initialize: initialize(request); break;
        case MessageType::ResolveMusicUrl: resolve(request); break;
        default: respondError(request.requestId, QStringLiteral("plugin.unsupported-message"),
                               QStringLiteral("The plugin runtime does not handle this message."));
        }
    }

private:
    struct PendingRequest {
        QString source;
        QString type;
    };

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
        const QByteArray source = file.readAll();

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

        engine_->globalObject().setProperty(QStringLiteral("__lf_bridge"), engine_->newQObject(bridge_.get()));
        const QJSValue bootstrap = engine_->evaluate(bootstrapScript, info.absoluteFilePath(), 1);
        if (engine_->hasError()) {
            const QJSValue error = engine_->catchError();
            reset();
            respondError(request.requestId, QStringLiteral("plugin.bootstrap-failed"), scriptError(error));
            return;
        }
        const QJSValue result = engine_->evaluate(QString::fromUtf8(source), info.absoluteFilePath(), 1);
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
        const QJsonObject musicInfo = request.payload.value(QStringLiteral("musicInfo")).toObject();
        const QJsonObject sourceInfo = initPayload_.value(QStringLiteral("sources")).toObject()
                                           .value(source).toObject();
        if (sourceInfo.isEmpty() || !containsString(sourceInfo.value(QStringLiteral("actions")).toArray(),
                                                     QStringLiteral("musicUrl")) ||
            !containsString(sourceInfo.value(QStringLiteral("qualitys")).toArray(), type) || musicInfo.isEmpty()) {
            respondError(request.requestId, QStringLiteral("plugin.invalid-request"),
                         QStringLiteral("Source, quality or musicInfo is not supported."));
            return;
        }
        pending_.insert(request.requestId, {source, type});
        QJsonObject info;
        info.insert(QStringLiteral("type"), type);
        info.insert(QStringLiteral("musicInfo"), musicInfo);
        QJsonObject event;
        event.insert(QStringLiteral("source"), source);
        event.insert(QStringLiteral("action"), QStringLiteral("musicUrl"));
        event.insert(QStringLiteral("info"), info);
        const QJSValue accepted = dispatch_.call({engine_->toScriptValue(request.requestId),
                                                  engine_->toScriptValue(QString::fromUtf8(
                                                      QJsonDocument(event).toJson(QJsonDocument::Compact)))});
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
        const QString url = parseError.error == QJsonParseError::NoError && value.isString()
                                ? value.toString()
                                : QString{};
        const QUrl parsed(url);
        if (url.isEmpty() || url.size() > MaxUrlBytes ||
            (parsed.scheme() != QStringLiteral("http") && parsed.scheme() != QStringLiteral("https")) ||
            parsed.host().isEmpty()) {
            respondError(requestId, QStringLiteral("plugin.invalid-url"),
                         QStringLiteral("Plugin returned an invalid media URL."));
            return;
        }
        respond(requestId, MessageType::Result,
                {{QStringLiteral("source"), pending.source},
                 {QStringLiteral("action"), QStringLiteral("musicUrl")},
                 {QStringLiteral("data"), QJsonObject{{QStringLiteral("type"), pending.type},
                                                        {QStringLiteral("url"), url}}}});
    }

    void handleRejected(const QString& requestId, const QString& message) {
        if (!pending_.remove(requestId)) return;
        respondError(requestId, QStringLiteral("plugin.request-failed"), message);
    }

    PluginRuntime& owner_;
    std::unique_ptr<QJSEngine> engine_;
    std::unique_ptr<PluginBridge> bridge_;
    QJSValue dispatch_;
    QJsonObject initPayload_;
    QHash<QString, PendingRequest> pending_;
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
