#include "infrastructure/database/database.h"
#include "infrastructure/database/repositories.h"
#include "infrastructure/library/library_scanner.h"
#include "qmlbridge/controllers.h"

#include <QDir>
#include <QGuiApplication>
#include <QStandardPaths>
#include <QTimer>
#include <QQmlApplicationEngine>
#include <QQmlContext>

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    const bool smokeMode = app.arguments().contains(QStringLiteral("--smoke"));

    listenfree::infrastructure::database::Database database;
    QString databasePath = QStringLiteral(":memory:");
    if (!smokeMode) {
        const auto dataDirectory = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        if (dataDirectory.isEmpty() || !QDir().mkpath(dataDirectory)) return 2;
        databasePath = QDir(dataDirectory).filePath(QStringLiteral("library.sqlite"));
    }
    if (!database.open(databasePath) || !database.migrate()) return 2;

    listenfree::infrastructure::database::TrackRepository trackRepository(database);
    listenfree::infrastructure::library::LocalLibraryScannerAdapter libraryScanner;
    listenfree::qmlbridge::AppController controller;
    listenfree::qmlbridge::LibraryController libraryController(libraryScanner, trackRepository);
    listenfree::qmlbridge::PlayerController playerController;
    QQmlApplicationEngine engine;
    engine.rootContext()->setContextProperty(QStringLiteral("appController"), &controller);
    engine.rootContext()->setContextProperty(QStringLiteral("libraryController"), &libraryController);
    engine.rootContext()->setContextProperty(QStringLiteral("playerController"), &playerController);
    QObject::connect(&engine, &QQmlApplicationEngine::objectCreationFailed, &app, [] { QCoreApplication::exit(1); },
                     Qt::QueuedConnection);
    engine.loadFromModule(QStringLiteral("ListenFree.Bootstrap"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) return 1;
    controller.initialize();
    if (smokeMode) {
        QTimer::singleShot(100, &app, &QCoreApplication::quit);
    }
    return app.exec();
}
