#include "qmlbridge/controllers.h"

#include <QGuiApplication>
#include <QTimer>
#include <QQmlApplicationEngine>
#include <QQmlContext>

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    listenfree::qmlbridge::AppController controller;
    listenfree::qmlbridge::PlayerController playerController;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("appController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("playerController"), &playerController);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); },
                     Qt::QueuedConnection);
    engine.loadFromModule(QStringLiteral("ListenFree.Bootstrap"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) return 1;
    controller.initialize();
    if (app.arguments().contains(QStringLiteral("--smoke"))) {
        QTimer::singleShot(100, &app, &QCoreApplication::quit);
    }
    return app.exec();
}
