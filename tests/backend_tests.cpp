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
#include <QThread>
#include <QtTest>

#include <taglib/wavfile.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <stdexcept>

class ThreadRecordingMetadataReader final : public listenfree::application::IMetadataReader {
public:
    std::optional<listenfree::domain::Track> read(const std::filesystem::path& path) override {
        readThread.store(QThread::currentThread());
        return fallback.read(path);
    }

    std::atomic<QThread*> readThread{nullptr};

private:
    listenfree::infrastructure::library::BasicMetadataReader fallback;
};

class ThrowingMetadataReader final : public listenfree::application::IMetadataReader {
public:
    std::optional<listenfree::domain::Track> read(const std::filesystem::path&) override {
        throw std::runtime_error("metadata-read-failed");
    }
};

class BackendTests final : public QObject {
    Q_OBJECT
private slots:
    void queueOperations();
    void queueRemovalPreservesCurrentItem();
    void playbackStateTransitions();
    void audioPlayerDomainAdapter();
    void audioPlayerMuteVolumeAndDynamicCapabilities();
    void playerControllerAdapter();
    void playerControllerProjectsMuteQueueAndLyrics();
    void databaseMigrationAndRepository();
    void databasePortRepositories();
    void metadataReaderMapsRegularFile();
    void metadataReaderAcceptsEmptyRegularFile();
    void metadataReaderRejectsMissingFile();
    void tagLibMetadataReaderFallsBackForInvalidMedia();
    void tagLibMetadataReaderMapsWavTags();
    void libraryScannerUsesBoundedBatchesOffOwnerThread();
    void libraryScannerAdapter();
    void libraryScannerAdapterCancellationIsTerminal();
    void libraryScannerAdapterFailureIsTerminal();
    void libraryScannerAdapterContainsBatchCallbackFailure();
    void libraryControllerPersistsScanBatches();
    void sourceProtocolRoundTrip();
    void sourceProtocolRejectsInvalidFrame();
    void sourceHostProcessLifecycle();
    void sourceHostRequestTimeout();
    void sourceHostCancelIsTerminal();
    void sourceHostStopCompletesPendingRequests();
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

void BackendTests::queueRemovalPreservesCurrentItem() {
    listenfree::domain::PlaybackQueue queue;
    for (const auto* id : {"first", "second", "third"}) {
        listenfree::domain::Track track;
        track.id = listenfree::domain::TrackId(id);
        QVERIFY(queue.enqueue({track, std::nullopt}));
    }
    QVERIFY(queue.select(1));
    QVERIFY(queue.remove(0));
    QCOMPARE(queue.currentIndex(), std::size_t(0));
    QCOMPARE(queue.items()[queue.currentIndex()].track.id.value(), std::string("second"));
}

void BackendTests::playbackStateTransitions() {
    using listenfree::media::BackendMediaStatus;
    using listenfree::media::BackendPlaybackState;
    using listenfree::media::PlaybackObservation;
    using listenfree::media::reducePlaybackState;

    QCOMPARE(reducePlaybackState({}).state, listenfree::domain::PlaybackState::Idle);
    QCOMPARE(reducePlaybackState({BackendMediaStatus::Loading,
                                  BackendPlaybackState::Stopped, true, false, false}).state,
             listenfree::domain::PlaybackState::Loading);
    QCOMPARE(reducePlaybackState({BackendMediaStatus::Buffering,
                                  BackendPlaybackState::Stopped, true, false, false}).state,
             listenfree::domain::PlaybackState::Buffering);
    QCOMPARE(reducePlaybackState({BackendMediaStatus::Stalled,
                                  BackendPlaybackState::Stopped, true, false, false, true}).state,
             listenfree::domain::PlaybackState::Buffering);
    QCOMPARE(reducePlaybackState({BackendMediaStatus::Loaded,
                                  BackendPlaybackState::Stopped, true, false, false, true}).state,
             listenfree::domain::PlaybackState::Loading);
    QCOMPARE(reducePlaybackState({BackendMediaStatus::Buffered,
                                  BackendPlaybackState::Playing, true, false, false}).state,
             listenfree::domain::PlaybackState::Playing);
    QCOMPARE(reducePlaybackState({BackendMediaStatus::Loaded,
                                  BackendPlaybackState::Paused, true, false, false}).state,
             listenfree::domain::PlaybackState::Paused);
    const auto ended = reducePlaybackState({BackendMediaStatus::EndOfMedia,
                                            BackendPlaybackState::Stopped, true, false, false});
    QCOMPARE(ended.state, listenfree::domain::PlaybackState::Stopped);
    QVERIFY(ended.finished);
    QCOMPARE(reducePlaybackState({BackendMediaStatus::Invalid,
                                  BackendPlaybackState::Stopped, true, true, false}).state,
             listenfree::domain::PlaybackState::Error);
    QCOMPARE(reducePlaybackState({BackendMediaStatus::Buffering,
                                  BackendPlaybackState::Stopped, true, false, true}).state,
             listenfree::domain::PlaybackState::Stopped);
}

void BackendTests::audioPlayerDomainAdapter() {
    listenfree::media::QtAudioPlayer player;
    const auto capabilities = player.capabilities();
    QVERIFY(capabilities & listenfree::application::capabilityMask(
                               listenfree::application::PlaybackCapability::LocalFile));
    QCOMPARE((capabilities & listenfree::application::capabilityMask(
                                  listenfree::application::PlaybackCapability::DeviceSelection)) != 0,
             !player.deviceIds().empty());
    QVERIFY(!(capabilities & listenfree::application::capabilityMask(
                                listenfree::application::PlaybackCapability::Equalizer)));
    QVERIFY(!player.supported());
    listenfree::domain::PlaybackItem item;
    item.track.id = listenfree::domain::TrackId("no-source");
    item.track.title = "Missing source";
    player.open(item);
    QCOMPARE(player.state(), listenfree::domain::PlaybackState::Idle);
}

void BackendTests::audioPlayerMuteVolumeAndDynamicCapabilities() {
    listenfree::media::QtAudioPlayer player;
    listenfree::application::IAudioPlayer& audioPlayer = player;

    audioPlayer.setVolume(-1.0F);
    QCOMPARE(audioPlayer.volume(), 0.0F);
    audioPlayer.setVolume(2.0F);
    QCOMPARE(audioPlayer.volume(), 1.0F);

    audioPlayer.setMuted(true);
    QVERIFY(audioPlayer.muted());
    audioPlayer.setMuted(false);
    QVERIFY(!audioPlayer.muted());

    const bool hasDevices = !player.deviceIds().empty();
    const bool reportsDeviceSelection =
        (player.capabilities() & listenfree::application::capabilityMask(
                                     listenfree::application::PlaybackCapability::DeviceSelection)) != 0;
    QCOMPARE(reportsDeviceSelection, hasDevices);
}

void BackendTests::playerControllerAdapter() {
    listenfree::qmlbridge::PlayerController controller;
    QCOMPARE(controller.state(), QStringLiteral("Idle"));
    controller.openLocal(QString());
    QCOMPARE(controller.state(), QStringLiteral("Idle"));
    controller.setVolume(2.0F);
    controller.seek(0);
}

void BackendTests::playerControllerProjectsMuteQueueAndLyrics() {
    listenfree::qmlbridge::PlayerController controller;
    controller.setMuted(true);
    QVERIFY(controller.muted());
    controller.setMuted(false);
    QVERIFY(!controller.muted());

    listenfree::domain::PlaybackItem first;
    first.track.id = listenfree::domain::TrackId("queue-first");
    first.track.title = "Queue First";
    listenfree::domain::PlaybackItem second;
    second.track.id = listenfree::domain::TrackId("queue-second");
    second.track.title = "Queue Second";
    controller.setQueue({first, second}, 1);
    QCOMPARE(controller.queueModel()->rowCount(), 2);
    QCOMPARE(controller.queueModel()->currentIndex(), 1);
    QCOMPARE(controller.currentTrackId(), QStringLiteral("queue-second"));

    controller.setLyrics({{std::chrono::milliseconds(0), std::chrono::milliseconds(1'000),
                           "first lyric"}});
    QCOMPARE(controller.lyricLineCount(), 1);
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

void BackendTests::libraryScannerUsesBoundedBatchesOffOwnerThread() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    constexpr int fileCount = 129;
    for (int index = 0; index < fileCount; ++index) {
        QFile file(directory.filePath(QStringLiteral("track-%1.mp3").arg(index)));
        QVERIFY(file.open(QIODevice::WriteOnly));
    }

    listenfree::infrastructure::library::LibraryScanner scanner;
    auto reader = std::make_shared<ThreadRecordingMetadataReader>();
    QVector<qsizetype> batchSizes;
    bool callbacksOnOwnerThread = true;
    connect(&scanner, &listenfree::infrastructure::library::LibraryScanner::tracksFound,
            &scanner, [&](QVector<listenfree::domain::Track> tracks) {
                callbacksOnOwnerThread = callbacksOnOwnerThread &&
                                         QThread::currentThread() == scanner.thread();
                batchSizes.push_back(tracks.size());
            });
    QSignalSpy finished(&scanner, &listenfree::infrastructure::library::LibraryScanner::finished);

    scanner.start({directory.path()}, reader);

    QVERIFY(finished.wait(5000));
    QCOMPARE(batchSizes, QVector<qsizetype>({64, 64, 1}));
    QVERIFY(callbacksOnOwnerThread);
    QVERIFY(reader->readThread.load() != nullptr);
    QVERIFY(reader->readThread.load() != scanner.thread());
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
    std::optional<listenfree::application::ScanOutcome> outcome;
    scanner.start(request, {
        [&](std::vector<listenfree::domain::Track> tracks) {
            found += static_cast<int>(tracks.size());
            if (!tracks.empty()) title = tracks.front().title;
        },
        [&](listenfree::application::ScanOutcome result) { outcome = std::move(result); }
    });
    QTRY_VERIFY_WITH_TIMEOUT(outcome.has_value(), 3000);
    QCOMPARE(outcome->status, listenfree::application::ScanStatus::Completed);
    QCOMPARE(found, 1);
    QCOMPARE(QString::fromStdString(title), QStringLiteral("demo"));
}

void BackendTests::libraryScannerAdapterCancellationIsTerminal() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QFile audio(directory.filePath(QStringLiteral("cancel.mp3")));
    QVERIFY(audio.open(QIODevice::WriteOnly));
    audio.close();

    listenfree::infrastructure::library::LocalLibraryScannerAdapter scanner(
        std::make_unique<listenfree::infrastructure::library::BasicMetadataReader>());
    listenfree::application::ScanRequest request;
    request.roots.emplace_back(directory.path().toStdWString());
    int terminalCalls = 0;
    listenfree::application::ScanStatus status = listenfree::application::ScanStatus::Completed;
    const auto id = scanner.start(request, {
        [](std::vector<listenfree::domain::Track>) {},
        [&](listenfree::application::ScanOutcome outcome) {
            ++terminalCalls;
            status = outcome.status;
        }
    });

    scanner.cancel(id + 1);
    QCOMPARE(terminalCalls, 0);
    scanner.cancel(id);
    QCOMPARE(terminalCalls, 1);
    QCOMPARE(status, listenfree::application::ScanStatus::Cancelled);
    scanner.cancel(id);
    QCOMPARE(terminalCalls, 1);
    QCoreApplication::processEvents();
    QCOMPARE(terminalCalls, 1);
}

void BackendTests::libraryScannerAdapterFailureIsTerminal() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QFile audio(directory.filePath(QStringLiteral("failure.mp3")));
    QVERIFY(audio.open(QIODevice::WriteOnly));
    audio.close();

    listenfree::infrastructure::library::LocalLibraryScannerAdapter scanner(
        std::make_unique<ThrowingMetadataReader>());
    listenfree::application::ScanRequest request;
    request.roots.emplace_back(directory.path().toStdWString());
    int terminalCalls = 0;
    std::optional<listenfree::application::ScanOutcome> result;
    scanner.start(request, {
        [](std::vector<listenfree::domain::Track>) {},
        [&](listenfree::application::ScanOutcome outcome) {
            ++terminalCalls;
            result = std::move(outcome);
        }
    });

    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 3000);
    QCOMPARE(terminalCalls, 1);
    QCOMPARE(result->status, listenfree::application::ScanStatus::Failed);
    QCOMPARE(result->error, std::string("metadata-read-failed"));
}

void BackendTests::libraryScannerAdapterContainsBatchCallbackFailure() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QFile audio(directory.filePath(QStringLiteral("callback.mp3")));
    QVERIFY(audio.open(QIODevice::WriteOnly));
    audio.close();

    listenfree::infrastructure::library::LocalLibraryScannerAdapter scanner(
        std::make_unique<listenfree::infrastructure::library::BasicMetadataReader>());
    listenfree::application::ScanRequest request;
    request.roots.emplace_back(directory.path().toStdWString());
    std::optional<listenfree::application::ScanOutcome> result;
    scanner.start(request, {
        [](std::vector<listenfree::domain::Track>) { throw std::runtime_error("batch-callback-failed"); },
        [&](listenfree::application::ScanOutcome outcome) { result = std::move(outcome); }
    });

    QTRY_VERIFY_WITH_TIMEOUT(result.has_value(), 3000);
    QCOMPARE(result->status, listenfree::application::ScanStatus::Failed);
    QCOMPARE(result->error, std::string("batch-callback-failed"));
}

void BackendTests::libraryControllerPersistsScanBatches() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QFile audio(directory.filePath(QStringLiteral("controller-track.mp3")));
    QVERIFY(audio.open(QIODevice::WriteOnly));
    audio.close();

    listenfree::infrastructure::database::Database database;
    QVERIFY(database.open(directory.filePath(QStringLiteral("controller.sqlite"))));
    listenfree::infrastructure::database::TrackRepository repository(database);
    listenfree::infrastructure::library::LocalLibraryScannerAdapter scanner(
        std::make_unique<listenfree::infrastructure::library::BasicMetadataReader>());
    listenfree::qmlbridge::LibraryController controller(scanner, repository);

    controller.scan({directory.path()});

    QTRY_VERIFY_WITH_TIMEOUT(!controller.scanning(), 3000);
    QCOMPARE(controller.importedCount(), quint64(1));
    QVERIFY(controller.lastError().isEmpty());
    const auto stored = repository.search("controller-track");
    QCOMPARE(stored.size(), std::size_t(1));
    QCOMPARE(stored.front().title, std::string("controller-track"));
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
    QSignalSpy readySpy(&client, &listenfree::sourcehost::SourceHostClient::ready);
    QSignalSpy finishedSpy(&client, &listenfree::sourcehost::SourceHostClient::requestFinished);
    QVERIFY(client.start());
    QVERIFY(client.running());
    QCOMPARE(readySpy.count(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 2000);
    QVERIFY(client.loadPlugin(std::filesystem::path("mock-source.js")));
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 1000);
    QCOMPARE(finishedSpy.takeFirst().at(1).value<listenfree::sourcehost::SourceHostClient::RequestTerminal>(),
             listenfree::sourcehost::SourceHostClient::RequestTerminal::Succeeded);
    client.stop();
    QTRY_VERIFY_WITH_TIMEOUT(!client.running(), 2000);
}

void BackendTests::sourceHostRequestTimeout() {
    const QString executable = QCoreApplication::applicationDirPath() + QStringLiteral("/listenfree-sourcehost-fault-host.exe");
    qputenv("LISTENFREE_FAULT_MODE", "hang");
    listenfree::sourcehost::SourceHostClient client(executable);
    QSignalSpy readySpy(&client, &listenfree::sourcehost::SourceHostClient::ready);
    QVERIFY(client.start());
    QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 2000);
    QSignalSpy timeoutSpy(&client, &listenfree::sourcehost::SourceHostClient::requestTimedOut);
    QSignalSpy finishedSpy(&client, &listenfree::sourcehost::SourceHostClient::requestFinished);
    listenfree::sourcehost::SourceMessage request;
    request.type = listenfree::sourcehost::MessageType::Search;
    request.requestId = QStringLiteral("timeout-1");
    request.payload.insert(QStringLiteral("query"), QStringLiteral("never-replied"));
    request.payload.insert(QStringLiteral("noReply"), true);
    QVERIFY(client.request(request, 50));
    QTRY_COMPARE_WITH_TIMEOUT(timeoutSpy.count(), 1, 1000);
    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(timeoutSpy.takeFirst().at(0).toString(), QStringLiteral("timeout-1"));
    QCOMPARE(finishedSpy.takeFirst().at(1).value<listenfree::sourcehost::SourceHostClient::RequestTerminal>(),
             listenfree::sourcehost::SourceHostClient::RequestTerminal::TimedOut);
    client.stop();
    qunsetenv("LISTENFREE_FAULT_MODE");
}

void BackendTests::sourceHostCancelIsTerminal() {
    const QString executable = QCoreApplication::applicationDirPath() + QStringLiteral("/listenfree-sourcehost-fault-host.exe");
    qputenv("LISTENFREE_FAULT_MODE", "hang");
    listenfree::sourcehost::SourceHostClient client(executable);
    QSignalSpy readySpy(&client, &listenfree::sourcehost::SourceHostClient::ready);
    QSignalSpy timeoutSpy(&client, &listenfree::sourcehost::SourceHostClient::requestTimedOut);
    QSignalSpy finishedSpy(&client, &listenfree::sourcehost::SourceHostClient::requestFinished);
    QVERIFY(client.start());
    QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 2000);

    listenfree::sourcehost::SourceMessage request;
    request.type = listenfree::sourcehost::MessageType::Search;
    request.requestId = QStringLiteral("cancel-1");
    request.payload.insert(QStringLiteral("noReply"), true);
    QVERIFY(client.request(request, 100));
    client.cancel("cancel-1");
    client.cancel("cancel-1");

    QCOMPARE(finishedSpy.count(), 1);
    QCOMPARE(finishedSpy.takeFirst().at(1).value<listenfree::sourcehost::SourceHostClient::RequestTerminal>(),
             listenfree::sourcehost::SourceHostClient::RequestTerminal::Cancelled);
    QTest::qWait(150);
    QCOMPARE(timeoutSpy.count(), 0);
    QCOMPARE(finishedSpy.count(), 0);
    client.stop();
    qunsetenv("LISTENFREE_FAULT_MODE");
}

void BackendTests::sourceHostStopCompletesPendingRequests() {
    const QString executable = QCoreApplication::applicationDirPath() + QStringLiteral("/listenfree-sourcehost-fault-host.exe");
    qputenv("LISTENFREE_FAULT_MODE", "hang");
    listenfree::sourcehost::SourceHostClient client(executable);
    QSignalSpy readySpy(&client, &listenfree::sourcehost::SourceHostClient::ready);
    QSignalSpy finishedSpy(&client, &listenfree::sourcehost::SourceHostClient::requestFinished);
    QVERIFY(client.start());
    QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 2000);

    for (int index = 0; index < 2; ++index) {
        listenfree::sourcehost::SourceMessage request;
        request.type = listenfree::sourcehost::MessageType::Search;
        request.requestId = QStringLiteral("stop-pending-%1").arg(index);
        request.payload.insert(QStringLiteral("noReply"), true);
        QVERIFY(client.request(request, 5000));
    }
    client.stop();

    QCOMPARE(finishedSpy.count(), 2);
    for (const auto& arguments : finishedSpy) {
        QCOMPARE(arguments.at(1).value<listenfree::sourcehost::SourceHostClient::RequestTerminal>(),
                 listenfree::sourcehost::SourceHostClient::RequestTerminal::HostStopped);
    }
    QTRY_VERIFY_WITH_TIMEOUT(!client.running(), 2000);
    QCOMPARE(finishedSpy.count(), 2);
    qunsetenv("LISTENFREE_FAULT_MODE");
}

void BackendTests::sourceHostCrashRecovery() {
    const QString executable = QCoreApplication::applicationDirPath() + QStringLiteral("/listenfree-sourcehost-fault-host.exe");
    qputenv("LISTENFREE_FAULT_MODE", "crash");
    listenfree::sourcehost::SourceHostClient client(executable);
    QSignalSpy crashedSpy(&client, &listenfree::sourcehost::SourceHostClient::crashed);
    QSignalSpy restartedSpy(&client, &listenfree::sourcehost::SourceHostClient::restarted);
    QSignalSpy readySpy(&client, &listenfree::sourcehost::SourceHostClient::ready);
    QVERIFY(client.start());
    QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 2000);
    listenfree::sourcehost::SourceMessage request;
    request.type = listenfree::sourcehost::MessageType::Log;
    request.requestId = QStringLiteral("crash-1");
    request.payload.insert(QStringLiteral("crash"), true);
    QVERIFY(client.request(request, 1000));
    QTRY_COMPARE_WITH_TIMEOUT(crashedSpy.count(), 1, 2000);
    QTRY_COMPARE_WITH_TIMEOUT(restartedSpy.count(), 1, 3000);
    QVERIFY(client.running());
    client.stop();
    qunsetenv("LISTENFREE_FAULT_MODE");
}

void BackendTests::sourceHostStopPreventsRestart() {
    const QString executable = QCoreApplication::applicationDirPath() + QStringLiteral("/listenfree-sourcehost-fault-host.exe");
    qputenv("LISTENFREE_FAULT_MODE", "crash");
    listenfree::sourcehost::SourceHostClient client(executable);
    QSignalSpy crashedSpy(&client, &listenfree::sourcehost::SourceHostClient::crashed);
    QSignalSpy restartedSpy(&client, &listenfree::sourcehost::SourceHostClient::restarted);
    connect(&client, &listenfree::sourcehost::SourceHostClient::crashed, &client,
            &listenfree::sourcehost::SourceHostClient::stop);
    QSignalSpy readySpy(&client, &listenfree::sourcehost::SourceHostClient::ready);
    QVERIFY(client.start());
    QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 2000);
    listenfree::sourcehost::SourceMessage request;
    request.type = listenfree::sourcehost::MessageType::Log;
    request.requestId = QStringLiteral("crash-stop");
    request.payload.insert(QStringLiteral("crash"), true);
    QVERIFY(client.request(request, 1000));
    QTRY_COMPARE_WITH_TIMEOUT(crashedSpy.count(), 1, 2000);
    QTest::qWait(100);
    QCOMPARE(restartedSpy.count(), 0);
    QVERIFY(!client.running());
    qunsetenv("LISTENFREE_FAULT_MODE");
}

void BackendTests::sourceHostBoundsPendingRequests() {
    const QString executable = QCoreApplication::applicationDirPath() + QStringLiteral("/listenfree-sourcehost-fault-host.exe");
    qputenv("LISTENFREE_FAULT_MODE", "hang");
    listenfree::sourcehost::SourceHostClient client(executable);
    QSignalSpy protocolSpy(&client, &listenfree::sourcehost::SourceHostClient::protocolError);
    QSignalSpy readySpy(&client, &listenfree::sourcehost::SourceHostClient::ready);
    QVERIFY(client.start());
    QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 2000);
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
    QTRY_VERIFY_WITH_TIMEOUT(!client.running(), 2000);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    QCOMPARE(client.findChildren<QTimer*>().size(), 0);
    qunsetenv("LISTENFREE_FAULT_MODE");
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
