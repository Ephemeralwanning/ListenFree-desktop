#include "domain/domain.h"
#include "infrastructure/database/database.h"
#include "infrastructure/database/repositories.h"
#include "infrastructure/library/library_scanner.h"
#include "media/playback_state_machine.h"
#include "media/qt_audio_player.h"
#include "online/mock_online_provider.h"
#include "qmlbridge/controllers.h"
#include "sourcehost/source_protocol.h"
#include "sourcehost/sourcehost_client.h"

#include <QCoreApplication>
#include <QDataStream>
#include <QFileInfo>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <taglib/wavfile.h>

#include <array>
#include <filesystem>

class BackendTests final : public QObject {
    Q_OBJECT
private slots:
    void queueOperations();
    void playbackStateTransitions();
    void audioPlayerDomainAdapter();
    void playerControllerAdapter();
    void databaseMigrationAndRepository();
    void databasePortRepositories();
    void metadataReaderMapsRegularFile();
    void metadataReaderAcceptsEmptyRegularFile();
    void metadataReaderRejectsMissingFile();
    void tagLibMetadataReaderFallsBackForInvalidMedia();
    void tagLibMetadataReaderMapsWavTags();
    void libraryScannerAdapter();
    void sourceProtocolRoundTrip();
    void sourceProtocolRejectsInvalidFrame();
    void sourceHostProcessLifecycle();
    void sourceHostRequestTimeout();
    void sourceHostCrashRecovery();
    void sourceHostStopPreventsRestart();
    void sourceHostBoundsPendingRequests();
    void mockProvider();
    void listModels();
    void appControllerMock();
};

void BackendTests::queueOperations() {
    listenfree::domain::PlaybackQueue queue;
    listenfree::domain::Track track;
    track.id = listenfree::domain::TrackId("track-1");
    track.title = "Track";
    QVERIFY(queue.enqueue({track, std::nullopt}));
    QVERIFY(queue.enqueue({track, std::nullopt}));
    QCOMPARE(queue.items().size(), std::size_t(2));
    QVERIFY(queue.select(1));
    QVERIFY(!queue.next());
    QVERIFY(queue.remove(1));
    QCOMPARE(queue.currentIndex(), std::size_t(0));
    QVERIFY(!queue.select(5));
}

void BackendTests::playbackStateTransitions() {
    listenfree::media::PlaybackStateMachine machine;
    QCOMPARE(machine.state(), listenfree::domain::PlaybackState::Idle);
    QVERIFY(machine.transition(listenfree::domain::PlaybackState::Loading));
    QVERIFY(machine.transition(listenfree::domain::PlaybackState::Playing));
    QVERIFY(machine.transition(listenfree::domain::PlaybackState::Paused));
    QVERIFY(!machine.transition(listenfree::domain::PlaybackState::Buffering));
    QVERIFY(machine.transition(listenfree::domain::PlaybackState::Stopped));
}

void BackendTests::audioPlayerDomainAdapter() {
    listenfree::media::QtAudioPlayer player;
    const auto capabilities = player.capabilities();
    QVERIFY(capabilities & listenfree::application::capabilityMask(
                               listenfree::application::PlaybackCapability::LocalFile));
    QVERIFY(capabilities & listenfree::application::capabilityMask(
                               listenfree::application::PlaybackCapability::DeviceSelection));
    QVERIFY(!(capabilities & listenfree::application::capabilityMask(
                                listenfree::application::PlaybackCapability::Equalizer)));
    QVERIFY(!player.supported());
    listenfree::domain::PlaybackItem item;
    item.track.id = listenfree::domain::TrackId("no-source");
    item.track.title = "Missing source";
    player.open(item);
    QCOMPARE(player.state(), listenfree::domain::PlaybackState::Idle);
}

void BackendTests::playerControllerAdapter() {
    listenfree::qmlbridge::PlayerController controller;
    QCOMPARE(controller.state(), QStringLiteral("Idle"));
    controller.openLocal(QString());
    QCOMPARE(controller.state(), QStringLiteral("Idle"));
    controller.setVolume(2.0F);
    controller.seek(0);
}

void BackendTests::databaseMigrationAndRepository() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    listenfree::infrastructure::database::Database database;
    QVERIFY(database.open(temp.filePath(QStringLiteral("library.sqlite"))));
    QVERIFY(database.isOpen());
    listenfree::domain::Track track;
    track.id = listenfree::domain::TrackId("db-track");
    track.title = "Database Track";
    track.duration = std::chrono::seconds(42);
    QVERIFY(database.upsertTrack(track));
    const auto tracks = database.loadTracks();
    QCOMPARE(tracks.size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(tracks.front().title), QStringLiteral("Database Track"));
    database.close();
    QVERIFY(!database.isOpen());
}

void BackendTests::databasePortRepositories() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    listenfree::infrastructure::database::Database database;
    QVERIFY(database.open(temp.filePath(QStringLiteral("ports.sqlite"))));
    QVERIFY(database.migrate());
    listenfree::infrastructure::database::TrackRepository tracks(database);
    listenfree::infrastructure::database::SettingsRepository settings(database);
    listenfree::infrastructure::database::PlaylistRepository playlists(database);

    listenfree::domain::Track first;
    first.id = listenfree::domain::TrackId("repo-1");
    first.title = "Repository Song";
    listenfree::domain::Track second;
    second.id = listenfree::domain::TrackId("repo-2");
    second.title = "Another Song";
    const std::array batch{first, second};
    QVERIFY(tracks.upsert(batch));
    QVERIFY(tracks.find(first.id).has_value());
    QCOMPARE(tracks.search("Repository").size(), std::size_t(1));
    QVERIFY(settings.set("volume", "0.75"));
    QCOMPARE(settings.get("volume").value_or(""), std::string("0.75"));
    QVERIFY(!settings.get("missing").has_value());

    listenfree::domain::Playlist playlist;
    playlist.id = listenfree::domain::PlaylistId("playlist-1");
    playlist.title = "Favorites";
    playlist.entries.push_back({"entry-1", first.id, 0});
    QVERIFY(playlists.save(playlist));
    const auto stored = playlists.list();
    QCOMPARE(stored.size(), std::size_t(1));
    QCOMPARE(stored.front().entries.size(), std::size_t(1));
    QVERIFY(playlists.remove(playlist.id));
    QVERIFY(playlists.list().empty());

    listenfree::domain::Playlist invalid;
    invalid.id = listenfree::domain::PlaylistId("playlist-invalid");
    invalid.title = "Should Roll Back";
    invalid.entries.push_back({"entry-invalid", listenfree::domain::TrackId("missing-track"), 0});
    QVERIFY(!playlists.save(invalid));
    QVERIFY(playlists.list().empty());
}

void BackendTests::metadataReaderMapsRegularFile() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    QFile file(temp.filePath(QStringLiteral("reader-track.mp3")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("placeholder-audio"), qint64(17));
    file.close();

    listenfree::infrastructure::library::BasicMetadataReader basicReader;
    listenfree::application::IMetadataReader& reader = basicReader;
    const auto track = reader.read(std::filesystem::path(file.fileName().toStdWString()));

    QVERIFY(track.has_value());
    QVERIFY(!track->id.empty());
    QCOMPARE(QString::fromStdString(track->title), QStringLiteral("reader-track"));
    QVERIFY(track->localPath.has_value());
    QCOMPARE(QFileInfo(QString::fromStdString(*track->localPath)).canonicalFilePath(),
             QFileInfo(file.fileName()).canonicalFilePath());
}

void BackendTests::metadataReaderAcceptsEmptyRegularFile() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    QFile file(temp.filePath(QStringLiteral("empty-track.flac")));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.close();

    listenfree::infrastructure::library::BasicMetadataReader basicReader;
    listenfree::application::IMetadataReader& reader = basicReader;
    const auto track = reader.read(std::filesystem::path(file.fileName().toStdWString()));

    QVERIFY(track.has_value());
    QCOMPARE(QString::fromStdString(track->title), QStringLiteral("empty-track"));
    QVERIFY(track->localPath.has_value());
}

void BackendTests::metadataReaderRejectsMissingFile() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const auto missing = std::filesystem::path(
        temp.filePath(QStringLiteral("missing-track.ogg")).toStdWString());

    listenfree::infrastructure::library::BasicMetadataReader basicReader;
    listenfree::application::IMetadataReader& reader = basicReader;

    QVERIFY(!reader.read(missing).has_value());
}

void BackendTests::tagLibMetadataReaderFallsBackForInvalidMedia() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("fallback.mp3"));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("not-a-media-file"), 16);
    file.close();

    listenfree::infrastructure::library::TagLibMetadataReader reader;
    const auto track = reader.read(std::filesystem::path(path.toStdWString()));

    QVERIFY(track.has_value());
    QCOMPARE(QString::fromStdString(track->title), QStringLiteral("fallback"));
    QCOMPARE(QString::fromStdString(track->localPath.value_or("")), QDir::toNativeSeparators(path));
    QVERIFY(track->artists.empty());
    QVERIFY(!track->album.has_value());
    QCOMPARE(track->duration, std::chrono::milliseconds(0));
}

void BackendTests::tagLibMetadataReaderMapsWavTags() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("tagged.wav"));

    constexpr quint32 sampleRate = 8000;
    constexpr quint32 dataSize = sampleRate;
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    QCOMPARE(stream.writeRawData("RIFF", 4), 4);
    stream << quint32(36 + dataSize);
    QCOMPARE(stream.writeRawData("WAVEfmt ", 8), 8);
    stream << quint32(16) << quint16(1) << quint16(1) << sampleRate << sampleRate
           << quint16(1) << quint16(8);
    QCOMPARE(stream.writeRawData("data", 4), 4);
    stream << dataSize;
    const QByteArray silence(static_cast<qsizetype>(dataSize), char(128));
    QCOMPARE(stream.writeRawData(silence.constData(), static_cast<qsizetype>(dataSize)),
             static_cast<int>(dataSize));
    QCOMPARE(stream.status(), QDataStream::Ok);
    file.close();

    {
        TagLib::RIFF::WAV::File taggedFile(path.toStdWString().c_str());
        QVERIFY(taggedFile.isValid());
        QVERIFY(taggedFile.tag() != nullptr);
        taggedFile.tag()->setTitle(TagLib::String("Tagged title", TagLib::String::UTF8));
        taggedFile.tag()->setArtist(TagLib::String("Tagged artist", TagLib::String::UTF8));
        taggedFile.tag()->setAlbum(TagLib::String("Tagged album", TagLib::String::UTF8));
        QVERIFY(taggedFile.save());
    }

    listenfree::infrastructure::library::TagLibMetadataReader reader;
    const auto track = reader.read(std::filesystem::path(path.toStdWString()));

    QVERIFY(track.has_value());
    QCOMPARE(QString::fromStdString(track->title), QStringLiteral("Tagged title"));
    QCOMPARE(track->artists.size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(track->artists.front().name), QStringLiteral("Tagged artist"));
    QVERIFY(track->album.has_value());
    QCOMPARE(QString::fromStdString(track->album->title), QStringLiteral("Tagged album"));
    QCOMPARE(track->duration, std::chrono::seconds(1));
    QVERIFY2(QFile::remove(path), "Metadata reader retained an open file handle");
}

void BackendTests::libraryScannerAdapter() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    QFile audio(temp.filePath(QStringLiteral("demo.mp3")));
    QVERIFY(audio.open(QIODevice::WriteOnly));
    audio.write("not-audio");
    audio.close();
    QFile ignored(temp.filePath(QStringLiteral("ignore.txt")));
    QVERIFY(ignored.open(QIODevice::WriteOnly));
    ignored.close();

    listenfree::infrastructure::library::LocalLibraryScannerAdapter scanner;
    listenfree::application::ScanRequest request;
    request.roots.emplace_back(temp.path().toStdWString());
    int found = 0;
    std::string title;
    scanner.start(request, [&](listenfree::domain::Track track) {
        ++found;
        title = track.title;
    }, [](std::string) {}, [&] { return false; });
    QTRY_COMPARE_WITH_TIMEOUT(found, 1, 3000);
    QCOMPARE(QString::fromStdString(title), QStringLiteral("demo"));
    scanner.cancel();
}

void BackendTests::sourceProtocolRoundTrip() {
    listenfree::sourcehost::SourceMessage input;
    input.type = listenfree::sourcehost::MessageType::Search;
    input.requestId = QStringLiteral("r1");
    input.payload = {{QStringLiteral("query"), QStringLiteral("test")}};
    const auto encoded = listenfree::sourcehost::SourceProtocol::encode(input);
    listenfree::sourcehost::SourceMessage output;
    QString error;
    QVERIFY(listenfree::sourcehost::SourceProtocol::decode(encoded, output, &error));
    QCOMPARE(output.protocolVersion, 1);
    QCOMPARE(output.type, listenfree::sourcehost::MessageType::Search);
    QCOMPARE(output.requestId, QStringLiteral("r1"));
    QCOMPARE(output.payload.value(QStringLiteral("query")).toString(), QStringLiteral("test"));
}

void BackendTests::sourceProtocolRejectsInvalidFrame() {
    listenfree::sourcehost::SourceMessage output;
    QString error;
    QVERIFY(!listenfree::sourcehost::SourceProtocol::decode(QByteArray("bad"), output, &error));
    QCOMPARE(error, QStringLiteral("frame-too-short"));
    listenfree::sourcehost::SourceMessage incompatible;
    incompatible.protocolVersion = 2;
    incompatible.requestId = QStringLiteral("version-2");
    const auto encoded = listenfree::sourcehost::SourceProtocol::encode(incompatible);
    QVERIFY(!listenfree::sourcehost::SourceProtocol::decode(encoded, output, &error));
    QCOMPARE(error, QStringLiteral("unsupported-protocol-version"));
}

void BackendTests::sourceHostProcessLifecycle() {
    const QString executable = QCoreApplication::applicationDirPath() + QStringLiteral("/listenfree-sourcehost.exe");
    QVERIFY(QFileInfo::exists(executable));
    listenfree::sourcehost::SourceHostClient client(executable);
    QVERIFY(client.start());
    QVERIFY(client.running());
    QVERIFY(client.loadPlugin(std::filesystem::path("mock-source.js")));
    client.cancel("request-1");
    client.stop();
    QVERIFY(!client.running());
}

void BackendTests::sourceHostRequestTimeout() {
    const QString executable = QCoreApplication::applicationDirPath() + QStringLiteral("/listenfree-sourcehost.exe");
    listenfree::sourcehost::SourceHostClient client(executable);
    QVERIFY(client.start());
    QSignalSpy timeoutSpy(&client, &listenfree::sourcehost::SourceHostClient::requestTimedOut);
    listenfree::sourcehost::SourceMessage request;
    request.type = listenfree::sourcehost::MessageType::Search;
    request.requestId = QStringLiteral("timeout-1");
    request.payload.insert(QStringLiteral("query"), QStringLiteral("never-replied"));
    request.payload.insert(QStringLiteral("noReply"), true);
    QVERIFY(client.request(request, 50));
    QTRY_COMPARE_WITH_TIMEOUT(timeoutSpy.count(), 1, 1000);
    QCOMPARE(timeoutSpy.takeFirst().at(0).toString(), QStringLiteral("timeout-1"));
    client.stop();
}

void BackendTests::sourceHostCrashRecovery() {
    const QString executable = QCoreApplication::applicationDirPath() + QStringLiteral("/listenfree-sourcehost.exe");
    listenfree::sourcehost::SourceHostClient client(executable);
    QSignalSpy crashedSpy(&client, &listenfree::sourcehost::SourceHostClient::crashed);
    QSignalSpy restartedSpy(&client, &listenfree::sourcehost::SourceHostClient::restarted);
    QVERIFY(client.start());
    listenfree::sourcehost::SourceMessage request;
    request.type = listenfree::sourcehost::MessageType::Log;
    request.requestId = QStringLiteral("crash-1");
    request.payload.insert(QStringLiteral("crash"), true);
    QVERIFY(client.request(request, 1000));
    QTRY_COMPARE_WITH_TIMEOUT(crashedSpy.count(), 1, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(restartedSpy.count(), 1, 3000);
    QVERIFY(client.running());
    client.stop();
}

void BackendTests::sourceHostStopPreventsRestart() {
    const QString executable = QCoreApplication::applicationDirPath() + QStringLiteral("/listenfree-sourcehost.exe");
    listenfree::sourcehost::SourceHostClient client(executable);
    QSignalSpy crashedSpy(&client, &listenfree::sourcehost::SourceHostClient::crashed);
    QSignalSpy restartedSpy(&client, &listenfree::sourcehost::SourceHostClient::restarted);
    connect(&client, &listenfree::sourcehost::SourceHostClient::crashed, &client,
            &listenfree::sourcehost::SourceHostClient::stop);
    QVERIFY(client.start());
    listenfree::sourcehost::SourceMessage request;
    request.type = listenfree::sourcehost::MessageType::Log;
    request.requestId = QStringLiteral("crash-stop");
    request.payload.insert(QStringLiteral("crash"), true);
    QVERIFY(client.request(request, 1000));
    QTRY_COMPARE_WITH_TIMEOUT(crashedSpy.count(), 1, 2000);
    QTest::qWait(100);
    QCOMPARE(restartedSpy.count(), 0);
    QVERIFY(!client.running());
}

void BackendTests::sourceHostBoundsPendingRequests() {
    const QString executable = QCoreApplication::applicationDirPath() + QStringLiteral("/listenfree-sourcehost.exe");
    listenfree::sourcehost::SourceHostClient client(executable);
    QSignalSpy protocolSpy(&client, &listenfree::sourcehost::SourceHostClient::protocolError);
    QVERIFY(client.start());
    for (int index = 0; index < 256; ++index) {
        listenfree::sourcehost::SourceMessage request;
        request.type = listenfree::sourcehost::MessageType::Search;
        request.requestId = QStringLiteral("pending-%1").arg(index);
        request.payload.insert(QStringLiteral("noReply"), true);
        QVERIFY(client.request(request, 5000));
    }
    listenfree::sourcehost::SourceMessage overflow;
    overflow.type = listenfree::sourcehost::MessageType::Search;
    overflow.requestId = QStringLiteral("pending-overflow");
    QVERIFY(!client.request(overflow, 5000));
    QCOMPARE(protocolSpy.takeLast().at(0).toString(), QStringLiteral("too-many-pending-requests"));
    client.stop();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(client.findChildren<QTimer*>().size(), 0);
}

void BackendTests::mockProvider() {
    listenfree::online::MockOnlineProvider provider;
    const auto results = provider.search("jazz");
    QCOMPARE(results.size(), std::size_t(1));
    QCOMPARE(QString::fromStdString(results.front().title), QStringLiteral("Mock jazz"));
    QCOMPARE(provider.playlists().size(), std::size_t(1));
}

void BackendTests::listModels() {
    listenfree::qmlbridge::TrackListModel model;
    listenfree::domain::Track track;
    track.id = listenfree::domain::TrackId("model-track");
    track.title = "Model Track";
    model.setTracks({track});
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0, 0), listenfree::qmlbridge::TrackListModel::TitleRole).toString(),
             QStringLiteral("Model Track"));
}

void BackendTests::appControllerMock() {
    listenfree::qmlbridge::AppController controller;
    QVERIFY(!controller.ready());
    controller.initialize();
    QVERIFY(controller.ready());
    QCOMPARE(controller.tracksModel()->rowCount(), 2);
    QCOMPARE(controller.queueModel()->rowCount(), 2);
    QCOMPARE(controller.playbackState(), QStringLiteral("Idle"));
}

QTEST_MAIN(BackendTests)
#include "backend_tests.moc"
