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
#include <QCryptographicHash>
#include <QDataStream>
#include <QFileInfo>
#include <QFile>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <taglib/wavfile.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <iterator>
#include <stdexcept>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

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

class CountingMetadataReader final : public listenfree::application::IMetadataReader {
public:
    std::optional<listenfree::domain::Track> read(const std::filesystem::path& path) override {
        ++readCount;
        return fallback.read(path);
    }

    int readCount{0};

private:
    listenfree::infrastructure::library::BasicMetadataReader fallback;
};

class ThrowingFingerprintRepository final : public listenfree::application::ITrackRepository {
public:
    bool upsert(std::span<const listenfree::domain::Track>) override { return true; }
    std::optional<listenfree::domain::Track> find(const listenfree::domain::TrackId&) override {
        return std::nullopt;
    }
    std::vector<listenfree::domain::Track> search(const std::string&) override { return {}; }
    std::vector<listenfree::application::LocalFileFingerprint> localFiles() override {
        throw std::runtime_error("fingerprint-load-failed");
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
    void databaseMigrationChecksumRejectsTampering();
    void databaseUpgradesShippedDuplicateAliasSchema();
    void databasePortRepositories();
    void databaseTrackRelationsRoundTrip();
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
    void libraryControllerContainsFingerprintFailure();
    void libraryControllerSkipsUnchangedSizeAndMtime();
    void libraryControllerPersistsAndUsesFolders();
    void libraryAutoWatchFindsSettledFilesAndHonorsSwitch();
    void libraryAutoWatchWaitsForManualScan();
    void settingsControllerPersistsValues();
    void libraryScannerExcludesSymbolicLinks();
    void libraryScannerDeduplicatesOverlappingRoots();
    void sourceProtocolRoundTrip();
    void sourceProtocolRejectsInvalidFrame();
    void sourceHostProcessLifecycle();
    void sourceHostLoadsCompressedPlugin();
    void sourceHostResolvesLyricAndPic();
    void sourceHostProvidesAsyncRequestBridge();
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

void BackendTests::databaseUpgradesShippedDuplicateAliasSchema() {
    using listenfree::infrastructure::database::Database;
    QTemporaryDir temp;QVERIFY(temp.isValid());
    const auto path=temp.filePath("shipped-v4.sqlite");
    Database database;QVERIFY(database.open(path));
    listenfree::domain::Track track;track.id=listenfree::domain::TrackId("keeper");track.title="Preserved track";
    QVERIFY(database.upsertTrack(track));
    QVERIFY(database.setSetting("portable.queue","{\"preserved\":true}"));
    QVERIFY(database.setSetting("collections.v1","{\"lists\":[1,2]}"));
    database.close();
    const QString oldSql="CREATE TABLE IF NOT EXISTS duplicate_aliases (path TEXT PRIMARY KEY, keeper_id TEXT NOT NULL, keeper_path TEXT NOT NULL, hash TEXT NOT NULL, size_bytes INTEGER NOT NULL, modified_ms INTEGER NOT NULL)";
    const auto oldChecksum=QString::fromLatin1(QCryptographicHash::hash(oldSql.toUtf8(),QCryptographicHash::Sha256).toHex());
    QCOMPARE(oldChecksum,QString("9730c9c84b3dd5e4c8d9783d056aa8483f9f287bc0abc8f8167b5f83a3198412"));
    const auto connectionName=QString("shipped-v4-fixture");
    {
        auto raw=QSqlDatabase::addDatabase("QSQLITE",connectionName);raw.setDatabaseName(path);QVERIFY(raw.open());
        QSqlQuery query(raw);
        QVERIFY(query.exec("DROP TABLE duplicate_aliases"));QVERIFY(query.exec(oldSql));
        QVERIFY(query.exec("INSERT INTO duplicate_aliases VALUES('duplicate.mp3','keeper','keeper.mp3','hash',123,456)"));
        QVERIFY(query.exec("INSERT INTO duplicate_aliases VALUES('stale.mp3','removed-track','removed.mp3','old-hash',11,22)"));
        query.prepare("UPDATE schema_migrations SET checksum=? WHERE version=4");query.addBindValue(oldChecksum);QVERIFY(query.exec());
        raw.close();
    }
    QSqlDatabase::removeDatabase(connectionName);
    QVERIFY2(database.open(path),"The previously shipped v4 database must remain startable");
    QVERIFY(database.migrate()); // main also calls migrate after open.
    QCOMPARE(database.loadTracks().size(),std::size_t(1));
    QCOMPARE(database.getSetting("portable.queue").value(),std::string("{\"preserved\":true}"));
    QCOMPARE(database.getSetting("collections.v1").value(),std::string("{\"lists\":[1,2]}"));
    database.close();QVERIFY(database.open(path));database.close();
    {
        auto raw=QSqlDatabase::addDatabase("QSQLITE",connectionName);raw.setDatabaseName(path);QVERIFY(raw.open());
        QSqlQuery query(raw);
        QVERIFY(query.exec("SELECT path,hash,size_bytes,modified_ms FROM duplicate_aliases"));QVERIFY(query.next());
        QCOMPARE(query.value(0).toString(),QString("duplicate.mp3"));QCOMPARE(query.value(1).toString(),QString("hash"));
        QCOMPARE(query.value(2).toInt(),123);QCOMPARE(query.value(3).toInt(),456);QVERIFY(!query.next());
        QVERIFY(query.exec("PRAGMA foreign_keys=ON"));
        QVERIFY(query.exec("DELETE FROM tracks WHERE track_id='keeper'"));
        QVERIFY(query.exec("SELECT COUNT(*) FROM duplicate_aliases"));QVERIFY(query.next());QCOMPARE(query.value(0).toInt(),0);
        QVERIFY(query.exec("UPDATE schema_migrations SET checksum='unrecognized-v4' WHERE version=4"));
        raw.close();
    }
    QSqlDatabase::removeDatabase(connectionName);
    QVERIFY(!database.open(path)); // The compatibility case does not disable validation.
}

void BackendTests::databaseMigrationChecksumRejectsTampering() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QString path = temp.filePath(QStringLiteral("checksum.sqlite"));
    listenfree::infrastructure::database::Database database;
    QVERIFY(database.open(path));
    database.close();

    const QString connectionName = QStringLiteral("checksum-tamper");
    {
        QSqlDatabase raw = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), connectionName);
        raw.setDatabaseName(path);
        QVERIFY(raw.open());
        QSqlQuery read(raw);
        QVERIFY(read.exec(QStringLiteral("SELECT checksum FROM schema_migrations WHERE version = 1")));
        QVERIFY(read.next());
        const QString checksum = read.value(0).toString();
        QCOMPARE(checksum.size(), 64);
        QVERIFY(checksum != QStringLiteral("bootstrap-v1"));
        QSqlQuery tamper(raw);
        QVERIFY(tamper.exec(QStringLiteral(
            "UPDATE schema_migrations SET checksum = 'tampered' WHERE version = 1")));
        raw.close();
    }
    QSqlDatabase::removeDatabase(connectionName);

    listenfree::infrastructure::database::Database reopened;
    QVERIFY(!reopened.open(path));
    QVERIFY(!reopened.isOpen());
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
    listenfree::infrastructure::database::PlayHistoryRepository history(database);

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

    listenfree::domain::Track atomicCandidate;
    atomicCandidate.id = listenfree::domain::TrackId("repo-atomic");
    atomicCandidate.title = "Must roll back";
    listenfree::domain::Track invalidCandidate;
    invalidCandidate.title = "Missing identifier";
    const std::array atomicBatch{atomicCandidate, invalidCandidate};
    QVERIFY(!tracks.upsert(atomicBatch));
    QVERIFY(!tracks.find(atomicCandidate.id).has_value());

    QVERIFY(history.record(first.id, std::chrono::system_clock::now()));
    QVERIFY(!history.record(listenfree::domain::TrackId("missing-history-track"),
                            std::chrono::system_clock::now()));
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

void BackendTests::databaseTrackRelationsRoundTrip() {
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    listenfree::infrastructure::database::Database database;
    QVERIFY(database.open(temp.filePath(QStringLiteral("relations.sqlite"))));
    listenfree::infrastructure::database::TrackRepository repository(database);
    listenfree::infrastructure::database::LibraryFolderRepository folders(database);

    listenfree::domain::Track track;
    track.id = listenfree::domain::TrackId("relations-track");
    track.title = "Relations";
    track.artists = {{"artist-1", "First Artist"}, {"artist-2", "Second Artist"}};
    track.album = listenfree::domain::Album{"album-1", "Album", std::string("https://art.invalid/a")};
    const std::array initial{track};
    QVERIFY(repository.upsert(initial));

    const auto stored = repository.find(track.id);
    QVERIFY(stored.has_value());
    QCOMPARE(stored->artists, track.artists);
    QCOMPARE(stored->album, track.album);

    track.artists = {{"artist-2", "Renamed Artist"}};
    track.album.reset();
    const std::array updated{track};
    QVERIFY(repository.upsert(updated));
    const auto replaced = repository.find(track.id);
    QVERIFY(replaced.has_value());
    QCOMPARE(replaced->artists, track.artists);
    QVERIFY(!replaced->album.has_value());
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
    listenfree::qmlbridge::LibraryController controller(scanner, repository, nullptr,
                                                         directory.filePath(QStringLiteral("controller.sqlite")));

    controller.scan({directory.path()});

    QTRY_VERIFY_WITH_TIMEOUT(!controller.scanning(), 3000);
    QCOMPARE(controller.importedCount(), quint64(1));
    QVERIFY(controller.lastError().isEmpty());
    const auto stored = repository.search("controller-track");
    QCOMPARE(stored.size(), std::size_t(1));
    QCOMPARE(stored.front().title, std::string("controller-track"));
}

void BackendTests::libraryControllerPersistsAndUsesFolders() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString root = QDir(directory.path()).canonicalPath();
    QFile audio(QDir(root).filePath(QStringLiteral("folder-track.mp3")));
    QVERIFY(audio.open(QIODevice::WriteOnly));
    audio.write("folder");
    audio.close();

    QTemporaryDir otherDirectory;
    QVERIFY(otherDirectory.isValid());
    const QString otherRoot = QDir(otherDirectory.path()).canonicalPath();
    QFile unrelatedAudio(QDir(otherRoot).filePath(QStringLiteral("unrelated-track.mp3")));
    QVERIFY(unrelatedAudio.open(QIODevice::WriteOnly));
    unrelatedAudio.write("unrelated");
    unrelatedAudio.close();

    listenfree::infrastructure::database::Database database;
    const QString databasePath = directory.filePath(QStringLiteral("folders.sqlite"));
    QVERIFY(database.open(databasePath));
    listenfree::infrastructure::database::TrackRepository tracks(database);
    listenfree::infrastructure::database::LibraryFolderRepository folders(database);
    listenfree::infrastructure::library::LocalLibraryScannerAdapter scanner(
        std::make_unique<listenfree::infrastructure::library::BasicMetadataReader>());
    listenfree::qmlbridge::LibraryController controller(scanner, tracks, &folders, databasePath);

    QVERIFY(controller.roots().isEmpty());
    QVERIFY(controller.addRoot(root));
    QCOMPARE(controller.roots(), QStringList({root}));
    QVERIFY(!controller.addRoot(QStringLiteral("C:/definitely-missing-listenfree-folder")));
    const QString nested = QDir(root).filePath(QStringLiteral("nested"));
    QVERIFY(QDir(nested).mkpath(QStringLiteral(".")));
    QVERIFY(!controller.addRoot(nested));
    QTRY_VERIFY_WITH_TIMEOUT(!controller.scanning(), 3000);
    controller.scan({otherRoot});
    QTRY_VERIFY_WITH_TIMEOUT(!controller.scanning(), 3000);
    QCOMPARE(controller.importedCount(), quint64(1));
    QCOMPARE(tracks.search("").size(), std::size_t(2));
    QCOMPARE(controller.totalCount(), quint64(2));

    QVERIFY(controller.removeRoot(root));
    QVERIFY(controller.roots().isEmpty());
    QCOMPARE(controller.totalCount(), quint64(1));
    QCOMPARE(tracks.search("unrelated-track").size(), std::size_t(1));
    QCOMPARE(tracks.search("folder-track").size(), std::size_t(0));
    QVERIFY(!controller.removeRoot(root));

    const QString missingRoot = QDir(root).filePath(QStringLiteral("missing"));
    QVERIFY(folders.add(std::filesystem::path(missingRoot.toStdWString())));
    QVERIFY(controller.removeRoot(missingRoot));
}

void BackendTests::libraryAutoWatchFindsSettledFilesAndHonorsSwitch() {
    QTemporaryDir directory;
    const QString root = directory.filePath("music");
    QVERIFY(QDir().mkpath(root));
    const auto write = [](const QString& path) {
        QFile file(path); return file.open(QIODevice::WriteOnly) && file.write("fixture") == 7;
    };
    QVERIFY(write(root + "/first.mp3"));
    listenfree::infrastructure::database::Database database;
    const auto databasePath = directory.filePath("watch.sqlite");
    QVERIFY(database.open(databasePath));
    listenfree::infrastructure::database::TrackRepository tracks(database);
    listenfree::infrastructure::database::LibraryFolderRepository folders(database);
    listenfree::infrastructure::library::LocalLibraryScannerAdapter scanner(
        std::make_unique<listenfree::infrastructure::library::BasicMetadataReader>());
    listenfree::qmlbridge::LibraryController controller(scanner, tracks, &folders, databasePath);
    QVERIFY(controller.addRoot(root));
    QTRY_VERIFY_WITH_TIMEOUT(!controller.scanning(), 3000);
    QCOMPARE(controller.totalCount(), 1);
    QVERIFY(write(root + "/while-off.mp3"));
    QTest::qWait(2700);
    QCOMPARE(controller.totalCount(), 1);
    controller.setAutoWatchEnabled(true);
    QTRY_COMPARE_WITH_TIMEOUT(controller.totalCount(), 2, 7000);

    const auto nested = root + "/new/album";
    QVERIFY(QDir().mkpath(nested));
    QVERIFY(write(nested + "/nested.flac"));
    QTRY_COMPARE_WITH_TIMEOUT(controller.totalCount(), 3, 7000);
    QFile slow(nested + "/slow.mp3");
    QVERIFY(slow.open(QIODevice::WriteOnly));
    for (int i = 0; i < 5; ++i) {
        QCOMPARE(slow.write(QByteArray(16384, 'a')), 16384);
        QVERIFY(slow.flush());
        QTest::qWait(650);
        QCOMPARE(controller.totalCount(), 3);
    }
    slow.close();
    QTRY_COMPARE_WITH_TIMEOUT(controller.totalCount(), 4, 7000);
    QVERIFY(write(nested + "/download.mp3.part"));
    QTest::qWait(2700);
    QCOMPARE(controller.totalCount(), 4);
    QVERIFY(QFile::rename(nested + "/download.mp3.part", nested + "/download.mp3"));
    controller.notifyFileCompleted(nested + "/download.mp3");
    QTRY_COMPARE_WITH_TIMEOUT(controller.totalCount(), 5, 7000);
    controller.notifyFileCompleted(nested + "/download.mp3");
    QTest::qWait(2700);
    QCOMPARE(controller.totalCount(), 5);

    controller.setAutoWatchEnabled(false);
    QVERIFY(write(nested + "/disabled.mp3"));
    QTest::qWait(2700);
    QCOMPARE(controller.totalCount(), 5);
    controller.setAutoWatchEnabled(true);
    QTRY_COMPARE_WITH_TIMEOUT(controller.totalCount(), 6, 7000);
    QVERIFY(write(nested + "/removed.mp3"));
    QVERIFY(controller.removeRoot(root));
    QTest::qWait(3000);
    QCOMPARE(controller.totalCount(), 0);
    QVERIFY(controller.lastError().isEmpty());
}

void BackendTests::libraryAutoWatchWaitsForManualScan() {
    using namespace listenfree;
    struct DeferredScanner final : application::ILocalLibraryScanner {
        application::ScanCallbacks callbacks;
        application::ScanRequest last;
        int starts=0, cancels=0;
        application::ScanId start(const application::ScanRequest& request, application::ScanCallbacks next) override {
            last=request;callbacks=std::move(next);return ++starts;
        }
        void cancel(application::ScanId) noexcept override { ++cancels; }
    } scanner;
    QTemporaryDir directory;
    const auto root=directory.filePath("music");QVERIFY(QDir().mkpath(root+"/album"));
    infrastructure::database::Database db;const auto path=directory.filePath("busy.sqlite");
    QVERIFY(db.open(path));
    infrastructure::database::TrackRepository tracks(db);
    infrastructure::database::LibraryFolderRepository folders(db);
    QVERIFY(folders.add(std::filesystem::path(root.toStdWString())));
    qmlbridge::LibraryController controller(scanner,tracks,&folders,path);
    controller.setAutoWatchEnabled(true);controller.cancel();controller.scanDefault();
    QTest::qWait(3200);
    QCOMPARE(scanner.starts,1);QCOMPARE(scanner.cancels,0);QVERIFY(controller.scanning());
    QFile added(root+"/album/new.mp3");QVERIFY(added.open(QIODevice::WriteOnly));added.write("fixture");added.close();
    QTest::qWait(3200);QCOMPARE(scanner.starts,1);QCOMPARE(scanner.cancels,0);
    scanner.callbacks.onFinished({application::ScanStatus::Completed,{}});
    QTRY_COMPARE_WITH_TIMEOUT(scanner.starts,2,1500);
    QVERIFY(!scanner.last.recursive);QCOMPARE(scanner.cancels,0);
    controller.setAutoWatchEnabled(false);
    QCOMPARE(scanner.cancels,1);QVERIFY(!controller.scanning());
    controller.scanDefault();controller.setAutoWatchEnabled(true);controller.setAutoWatchEnabled(false);
    QCOMPARE(scanner.cancels,1);QVERIFY(controller.scanning());
    scanner.callbacks.onFinished({application::ScanStatus::Completed,{}});
}

void BackendTests::settingsControllerPersistsValues() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    listenfree::infrastructure::database::Database database;
    QVERIFY(database.open(directory.filePath(QStringLiteral("settings.sqlite"))));
    listenfree::infrastructure::database::SettingsRepository repository(database);
    listenfree::qmlbridge::SettingsController controller(repository);

    QCOMPARE(controller.value(QStringLiteral("ui.language"), QStringLiteral("zh-CN")).toString(),
             QStringLiteral("zh-CN"));
    controller.setValue(QStringLiteral("ui.language"), QStringLiteral("en-US"));
    QCOMPARE(controller.value(QStringLiteral("ui.language")).toString(), QStringLiteral("en-US"));

    listenfree::qmlbridge::SettingsController restored(repository);
    QCOMPARE(restored.value(QStringLiteral("ui.language")).toString(), QStringLiteral("en-US"));
    controller.setValue(QStringLiteral("ui.motionEnabled"), false);
    const auto flag = restored.value(QStringLiteral("ui.motionEnabled"), true);
    QCOMPARE(flag.metaType(), QMetaType::fromType<bool>());
    QVERIFY(!flag.toBool());
    controller.setValue(QStringLiteral("lyrics.textSize"), 36);
    QCOMPARE(restored.value(QStringLiteral("lyrics.textSize"), 28).toInt(), 36);
}

void BackendTests::libraryControllerContainsFingerprintFailure() {
    listenfree::infrastructure::library::LocalLibraryScannerAdapter scanner;
    ThrowingFingerprintRepository repository;
    listenfree::qmlbridge::LibraryController controller(scanner, repository, nullptr, QString{});

    controller.scan({QStringLiteral("C:/library")});

    QVERIFY(!controller.scanning());
    QCOMPARE(controller.lastError(), QStringLiteral("fingerprint-load-failed"));
}

void BackendTests::libraryControllerSkipsUnchangedSizeAndMtime() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString path = directory.filePath(QStringLiteral("incremental.mp3"));
    QFile audio(path);
    QVERIFY(audio.open(QIODevice::WriteOnly));
    QCOMPARE(audio.write("first"), qint64(5));
    audio.close();

    listenfree::infrastructure::database::Database database;
    QVERIFY(database.open(directory.filePath(QStringLiteral("incremental.sqlite"))));
    listenfree::infrastructure::database::TrackRepository repository(database);
    listenfree::infrastructure::database::LibraryFolderRepository folders(database);
    auto reader = std::make_unique<CountingMetadataReader>();
    auto* readerProbe = reader.get();
    listenfree::infrastructure::library::LocalLibraryScannerAdapter scanner(std::move(reader));
    listenfree::qmlbridge::LibraryController controller(scanner, repository, &folders,
                                                         directory.filePath(QStringLiteral("incremental.sqlite")));

    controller.scan({directory.path()});
    QTRY_VERIFY_WITH_TIMEOUT(!controller.scanning(), 3000);
    QCOMPARE(readerProbe->readCount, 1);
    QCOMPARE(controller.importedCount(), quint64(1));
    QCOMPARE(repository.localFiles().size(), std::size_t(1));

    controller.scan({directory.path()});
    QTRY_VERIFY_WITH_TIMEOUT(!controller.scanning(), 3000);
    QCOMPARE(readerProbe->readCount, 1);
    QCOMPARE(controller.importedCount(), quint64(0));

    QVERIFY(audio.open(QIODevice::Append));
    QCOMPARE(audio.write("-changed"), qint64(8));
    audio.close();
    controller.scan({directory.path()});
    QTRY_VERIFY_WITH_TIMEOUT(!controller.scanning(), 3000);
    QCOMPARE(readerProbe->readCount, 2);
    QCOMPARE(controller.importedCount(), quint64(1));
    const auto fingerprints = repository.localFiles();
    QCOMPARE(fingerprints.size(), std::size_t(1));
    QCOMPARE(fingerprints.front().sizeBytes, std::uintmax_t(13));
}

void BackendTests::libraryScannerExcludesSymbolicLinks() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString realPath = directory.filePath(QStringLiteral("real.mp3"));
    QFile real(realPath);
    QVERIFY(real.open(QIODevice::WriteOnly));
    QCOMPARE(real.write("audio"), qint64(5));
    real.close();
    const auto linkPath = std::filesystem::path(
        directory.filePath(QStringLiteral("alias.mp3")).toStdWString());
#ifdef Q_OS_WIN
    const std::wstring target = std::filesystem::path(realPath.toStdWString()).wstring();
    const std::wstring link = linkPath.wstring();
    const BOOL linked = CreateSymbolicLinkW(link.c_str(), target.c_str(), 0x2U);
    QVERIFY2(linked, qPrintable(QStringLiteral("CreateSymbolicLinkW failed: %1")
                                   .arg(GetLastError())));
#else
    std::error_code error;
    std::filesystem::create_symlink(std::filesystem::path(realPath.toStdWString()), linkPath, error);
    QVERIFY2(!error, qPrintable(QString::fromStdString(error.message())));
#endif

    auto reader = std::make_unique<CountingMetadataReader>();
    auto* readerProbe = reader.get();
    listenfree::infrastructure::library::LocalLibraryScannerAdapter scanner(std::move(reader));
    listenfree::application::ScanRequest request;
    request.roots.emplace_back(directory.path().toStdWString());
    int tracks = 0;
    std::optional<listenfree::application::ScanOutcome> outcome;
    scanner.start(request, {
        [&](std::vector<listenfree::domain::Track> batch) { tracks += static_cast<int>(batch.size()); },
        [&](listenfree::application::ScanOutcome value) { outcome = std::move(value); }});
    QTRY_VERIFY_WITH_TIMEOUT(outcome.has_value(), 3000);
    QCOMPARE(outcome->status, listenfree::application::ScanStatus::Completed);
    QCOMPARE(tracks, 1);
    QCOMPARE(readerProbe->readCount, 1);
}

void BackendTests::libraryScannerDeduplicatesOverlappingRoots() {
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    QDir root(directory.path());
    QVERIFY(root.mkpath(QStringLiteral("nested")));
    QFile audio(root.filePath(QStringLiteral("nested/overlap.mp3")));
    QVERIFY(audio.open(QIODevice::WriteOnly));
    QCOMPARE(audio.write("audio"), qint64(5));
    audio.close();

    auto reader = std::make_unique<CountingMetadataReader>();
    auto* readerProbe = reader.get();
    listenfree::infrastructure::library::LocalLibraryScannerAdapter scanner(std::move(reader));
    listenfree::application::ScanRequest request;
    request.roots.emplace_back(directory.path().toStdWString());
    request.roots.emplace_back(root.filePath(QStringLiteral("nested")).toStdWString());
    int tracks = 0;
    std::optional<listenfree::application::ScanOutcome> outcome;
    scanner.start(request, {
        [&](std::vector<listenfree::domain::Track> batch) { tracks += static_cast<int>(batch.size()); },
        [&](listenfree::application::ScanOutcome value) { outcome = std::move(value); }});
    QTRY_VERIFY_WITH_TIMEOUT(outcome.has_value(), 3000);
    QCOMPARE(outcome->status, listenfree::application::ScanStatus::Completed);
    QCOMPARE(tracks, 1);
    QCOMPARE(readerProbe->readCount, 1);
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
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    QFile plugin(temp.filePath(QStringLiteral("mock-source.js")));
    QVERIFY(plugin.open(QIODevice::WriteOnly | QIODevice::Text));
    QVERIFY(plugin.write(R"JS(
lx.on(lx.EVENT_NAMES.request, ({ source, action, info }) => {
    if (action !== 'musicUrl') return Promise.reject(new Error('unsupported action'))
    return Promise.resolve('https://media.invalid/' + source + '/' + info.type + '/' + info.musicInfo.id + '.mp3')
})
lx.send(lx.EVENT_NAMES.inited, {
    status: true,
    sources: { kw: { type: 'music', actions: ['musicUrl'], qualitys: ['320k'] } }
})
)JS"));
    plugin.close();

    listenfree::sourcehost::SourceHostClient client(executable);
    QSignalSpy readySpy(&client, &listenfree::sourcehost::SourceHostClient::ready);
    QSignalSpy finishedSpy(&client, &listenfree::sourcehost::SourceHostClient::requestFinished);
    listenfree::sourcehost::SourceMessage lastMessage;
    connect(&client, &listenfree::sourcehost::SourceHostClient::messageReceived, &client,
            [&](const listenfree::sourcehost::SourceMessage& message) { lastMessage = message; });
    QVERIFY(client.start());
    QVERIFY(client.running());
    QCOMPARE(readySpy.count(), 0);
    QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 2000);
    listenfree::sourcehost::SourceMessage load;
    load.type = listenfree::sourcehost::MessageType::LoadPlugin;
    load.requestId = QStringLiteral("plugin-load");
    load.payload.insert(QStringLiteral("path"), plugin.fileName());
    QVERIFY(client.request(load, 2000));
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 1000);
    QCOMPARE(finishedSpy.takeFirst().at(1).value<listenfree::sourcehost::SourceHostClient::RequestTerminal>(),
             listenfree::sourcehost::SourceHostClient::RequestTerminal::Succeeded);
    QCOMPARE(lastMessage.type, listenfree::sourcehost::MessageType::Result);

    listenfree::sourcehost::SourceMessage initialize;
    initialize.type = listenfree::sourcehost::MessageType::Initialize;
    initialize.requestId = QStringLiteral("plugin-init");
    QVERIFY(client.request(initialize, 2000));
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 1000);
    QCOMPARE(finishedSpy.takeFirst().at(1).value<listenfree::sourcehost::SourceHostClient::RequestTerminal>(),
             listenfree::sourcehost::SourceHostClient::RequestTerminal::Succeeded);
    QCOMPARE(lastMessage.payload.value(QStringLiteral("sources")).toObject().value(QStringLiteral("kw"))
                 .toObject().value(QStringLiteral("type")).toString(), QStringLiteral("music"));

    listenfree::sourcehost::SourceMessage resolve;
    resolve.type = listenfree::sourcehost::MessageType::ResolveMusicUrl;
    resolve.requestId = QStringLiteral("plugin-resolve");
    resolve.payload.insert(QStringLiteral("source"), QStringLiteral("kw"));
    resolve.payload.insert(QStringLiteral("type"), QStringLiteral("320k"));
    resolve.payload.insert(QStringLiteral("musicInfo"),
                           QJsonObject{{QStringLiteral("id"), QStringLiteral("song-1")}});
    QVERIFY(client.request(resolve, 2000));
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 1000);
    QCOMPARE(finishedSpy.takeFirst().at(1).value<listenfree::sourcehost::SourceHostClient::RequestTerminal>(),
             listenfree::sourcehost::SourceHostClient::RequestTerminal::Succeeded);
    QCOMPARE(lastMessage.payload.value(QStringLiteral("data")).toObject().value(QStringLiteral("url")).toString(),
             QStringLiteral("https://media.invalid/kw/320k/song-1.mp3"));

    listenfree::sourcehost::SourceMessage unload;
    unload.type = listenfree::sourcehost::MessageType::UnloadPlugin;
    unload.requestId = QStringLiteral("plugin-unload");
    QVERIFY(client.request(unload, 2000));
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 1000);
    QCOMPARE(finishedSpy.takeFirst().at(1).value<listenfree::sourcehost::SourceHostClient::RequestTerminal>(),
             listenfree::sourcehost::SourceHostClient::RequestTerminal::Succeeded);
    listenfree::sourcehost::SourceMessage afterUnload = resolve;
    afterUnload.requestId = QStringLiteral("plugin-after-unload");
    QVERIFY(client.request(afterUnload, 2000));
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 1000);
    QCOMPARE(finishedSpy.takeFirst().at(1).value<listenfree::sourcehost::SourceHostClient::RequestTerminal>(),
             listenfree::sourcehost::SourceHostClient::RequestTerminal::RemoteError);
    client.stop();
    QTRY_VERIFY_WITH_TIMEOUT(!client.running(), 2000);
}

void BackendTests::sourceHostLoadsCompressedPlugin() {
    const QString executable = QCoreApplication::applicationDirPath() + QStringLiteral("/listenfree-sourcehost.exe");
    QVERIFY(QFileInfo::exists(executable));
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    const QByteArray source = R"JS(/*
 * @name Compressed source
 * @description compressed contract fixture
 * @version 1.0.0
 * @author ListenFree
 */
if (lx.currentScriptInfo.name !== 'Compressed source' || lx.currentScriptInfo.version !== '1.0.0') {
    throw new Error('metadata was not decoded')
}
lx.send(lx.EVENT_NAMES.inited, {
    status: true,
    sources: { kw: { type: 'music', actions: ['musicUrl'], qualitys: ['320k'] } }
})
)JS";
    // qCompress uses the same zlib stream format as the legacy Node zlib.deflate
    // helper; its four-byte Qt size prefix is not part of the stored payload.
    const QByteArray encoded = QByteArrayLiteral("gz_") + qCompress(source, 9).mid(4).toBase64();
    QFile plugin(temp.filePath(QStringLiteral("compressed-source.js")));
    QVERIFY(plugin.open(QIODevice::WriteOnly));
    QCOMPARE(plugin.write(encoded), encoded.size());
    plugin.close();

    listenfree::sourcehost::SourceHostClient client(executable);
    QSignalSpy readySpy(&client, &listenfree::sourcehost::SourceHostClient::ready);
    QSignalSpy finishedSpy(&client, &listenfree::sourcehost::SourceHostClient::requestFinished);
    listenfree::sourcehost::SourceMessage lastMessage;
    connect(&client, &listenfree::sourcehost::SourceHostClient::messageReceived, &client,
            [&](const listenfree::sourcehost::SourceMessage& message) { lastMessage = message; });
    QVERIFY(client.start());
    QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 2000);
    listenfree::sourcehost::SourceMessage load;
    load.type = listenfree::sourcehost::MessageType::LoadPlugin;
    load.requestId = QStringLiteral("compressed-load");
    load.payload.insert(QStringLiteral("path"), plugin.fileName());
    QVERIFY(client.request(load, 2000));
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 1000);
    QCOMPARE(finishedSpy.takeFirst().at(1).value<listenfree::sourcehost::SourceHostClient::RequestTerminal>(),
             listenfree::sourcehost::SourceHostClient::RequestTerminal::Succeeded);
    QCOMPARE(lastMessage.payload.value(QStringLiteral("sources")).toObject().value(QStringLiteral("kw"))
                 .toObject().value(QStringLiteral("qualitys")).toArray().size(), 1);
    client.stop();
    QTRY_VERIFY_WITH_TIMEOUT(!client.running(), 2000);
}

void BackendTests::sourceHostResolvesLyricAndPic() {
    const QString executable = QCoreApplication::applicationDirPath() + QStringLiteral("/listenfree-sourcehost.exe");
    QVERIFY(QFileInfo::exists(executable));
    QTemporaryDir temp;
    QVERIFY(temp.isValid());
    QFile plugin(temp.filePath(QStringLiteral("lyric-pic-source.js")));
    QVERIFY(plugin.open(QIODevice::WriteOnly | QIODevice::Text));
    QVERIFY(plugin.write(R"JS(
lx.on(lx.EVENT_NAMES.request, ({ action }) => {
    if (action === 'lyric') return Promise.resolve({ lyric: '[00:01.00]hello', tlyric: '你好', rlyric: 'romaji', lxlyric: 'extra' })
    if (action === 'pic') return Promise.resolve('https://media.invalid/cover.jpg')
    return Promise.reject(new Error('unsupported'))
})
lx.send(lx.EVENT_NAMES.inited, {
    status: true,
    sources: { local: { type: 'music', actions: ['musicUrl', 'lyric', 'pic'], qualitys: [] } }
})
)JS"));
    plugin.close();

    listenfree::sourcehost::SourceHostClient client(executable);
    QSignalSpy readySpy(&client, &listenfree::sourcehost::SourceHostClient::ready);
    QSignalSpy finishedSpy(&client, &listenfree::sourcehost::SourceHostClient::requestFinished);
    listenfree::sourcehost::SourceMessage lastMessage;
    connect(&client, &listenfree::sourcehost::SourceHostClient::messageReceived, &client,
            [&](const listenfree::sourcehost::SourceMessage& message) { lastMessage = message; });
    QVERIFY(client.start());
    QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 2000);
    listenfree::sourcehost::SourceMessage load;
    load.type = listenfree::sourcehost::MessageType::LoadPlugin;
    load.requestId = QStringLiteral("lyric-pic-load");
    load.payload.insert(QStringLiteral("path"), plugin.fileName());
    QVERIFY(client.request(load, 2000));
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 1000);
    QCOMPARE(finishedSpy.takeFirst().at(1).value<listenfree::sourcehost::SourceHostClient::RequestTerminal>(),
             listenfree::sourcehost::SourceHostClient::RequestTerminal::Succeeded);

    listenfree::sourcehost::SourceMessage lyric;
    lyric.type = listenfree::sourcehost::MessageType::ResolveLyric;
    lyric.requestId = QStringLiteral("lyric-request");
    lyric.payload.insert(QStringLiteral("source"), QStringLiteral("local"));
    lyric.payload.insert(QStringLiteral("musicInfo"), QJsonObject{{QStringLiteral("id"), QStringLiteral("song-2")}});
    QVERIFY(client.request(lyric, 2000));
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 1000);
    QCOMPARE(finishedSpy.takeFirst().at(1).value<listenfree::sourcehost::SourceHostClient::RequestTerminal>(),
             listenfree::sourcehost::SourceHostClient::RequestTerminal::Succeeded);
    QCOMPARE(lastMessage.payload.value(QStringLiteral("data")).toObject().value(QStringLiteral("lyric")).toString(),
             QStringLiteral("[00:01.00]hello"));

    listenfree::sourcehost::SourceMessage pic;
    pic.type = listenfree::sourcehost::MessageType::ResolvePic;
    pic.requestId = QStringLiteral("pic-request");
    pic.payload.insert(QStringLiteral("source"), QStringLiteral("local"));
    pic.payload.insert(QStringLiteral("musicInfo"), QJsonObject{{QStringLiteral("id"), QStringLiteral("song-2")}});
    QVERIFY(client.request(pic, 2000));
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 1000);
    QCOMPARE(finishedSpy.takeFirst().at(1).value<listenfree::sourcehost::SourceHostClient::RequestTerminal>(),
             listenfree::sourcehost::SourceHostClient::RequestTerminal::Succeeded);
    QCOMPARE(lastMessage.payload.value(QStringLiteral("data")).toString(),
             QStringLiteral("https://media.invalid/cover.jpg"));
    client.stop();
    QTRY_VERIFY_WITH_TIMEOUT(!client.running(), 2000);
}

void BackendTests::sourceHostProvidesAsyncRequestBridge() {
    const QString executable = QCoreApplication::applicationDirPath() + QStringLiteral("/listenfree-sourcehost.exe");
    QVERIFY(QFileInfo::exists(executable));
    QTemporaryDir temp;
    QVERIFY(temp.isValid());

    QTcpServer server;
    QVERIFY(server.listen(QHostAddress::LocalHost));
    bool hangingRequest = false;
    bool formRequestValid = false;
    bool multipartRequestValid = false;
    bool proxyRequestObserved = false;
    connect(&server, &QTcpServer::newConnection, &server,
            [&server, &hangingRequest, &formRequestValid, &multipartRequestValid,
             &proxyRequestObserved] {
        while (server.hasPendingConnections()) {
            QTcpSocket* socket = server.nextPendingConnection();
            connect(socket, &QTcpSocket::readyRead, socket,
                    [socket, &hangingRequest, &formRequestValid, &multipartRequestValid,
                     &proxyRequestObserved] {
                QByteArray request = socket->property("requestData").toByteArray();
                request += socket->readAll();
                socket->setProperty("requestData", request);
                const auto headerEnd = request.indexOf("\r\n\r\n");
                if (headerEnd < 0) return;
                if (request.left(headerEnd).contains("http://music.invalid")) {
                    proxyRequestObserved = true;
                }
                const auto headers = request.left(headerEnd).toLower();
                const QRegularExpression lengthExpression(QStringLiteral("content-length:\\s*(\\d+)"),
                                                           QRegularExpression::CaseInsensitiveOption);
                const auto lengthMatch = lengthExpression.match(QString::fromLatin1(headers));
                const auto contentLength = lengthMatch.hasMatch() ? lengthMatch.captured(1).toInt() : 0;
                if (request.size() < headerEnd + 4 + contentLength) return;
                if (request.contains("/hang")) {
                    hangingRequest = true;
                    return;
                }
                const auto bodyBytes = request.mid(headerEnd + 4, contentLength);
                if (request.contains("/form HTTP/1.1")) {
                    formRequestValid = headers.contains("content-type: application/x-www-form-urlencoded") &&
                                       bodyBytes.contains("alpha=one%20two") &&
                                       bodyBytes.contains("beta=%E4%B8%89");
                } else if (request.contains("/multipart HTTP/1.1")) {
                    multipartRequestValid = headers.contains("content-type: multipart/form-data; boundary=") &&
                                            bodyBytes.contains("name=\"field\"") &&
                                            bodyBytes.contains("value") &&
                                            bodyBytes.contains("name=\"binary\"") &&
                                            bodyBytes.contains("raw-bytes");
                }
                const QByteArray body = R"({"url":"https://media.invalid/from-request.mp3"})";
                const QByteArray response = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: " +
                                             QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
                socket->write(response);
                socket->disconnectFromHost();
            });
        }
    });

    const QString script = QStringLiteral(R"JS(
const { EVENT_NAMES, request, on, send } = globalThis.lx
on(EVENT_NAMES.request, ({ source, action, info }) => {
    if (action !== 'musicUrl') return Promise.reject(new Error('unsupported action'))
    const input = lx.utils.buffer.from('contract')
    if (lx.utils.crypto.md5('contract') !== '800c327aefb3f9241513cbf551abbfda') {
        return Promise.reject(new Error('md5 bridge failed'))
    }
    const encrypted = lx.utils.crypto.aesEncrypt(input, 'aes-128-cbc', lx.utils.crypto.randomBytes(16), lx.utils.crypto.randomBytes(16))
    if (!encrypted || encrypted.length !== 16) return Promise.reject(new Error('aes bridge failed'))
    const rsaKey = '-----BEGIN PUBLIC KEY-----\nMIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDgtQn2JZ34ZC28NWYpAUd98iZ37BUrX/aKzmFbt7clFSs6sXqHauqKWqdtLkF2KexO40H1YTX8z2lSgBBOAxLsvaklV8k4cBFK9snQXE9/DDaFt6Rr7iVZMldczhC0JNgTz+SHXT6CBHuX3e9SdB1Ua44oncaTWz7OBGLbCiK45wIDAQAB\n-----END PUBLIC KEY-----'
    const rsa = lx.utils.crypto.rsaEncrypt(lx.utils.buffer.from('1234567890abcdef'), rsaKey)
    if (rsa.toString('hex') !== '19cf95d2fd1442f7e8a134c41f3b495df622afba2bcab8aed511021d534cce4ca4367e5b8117759e2591c5117ef24fa8072addeb8179e5dcb2cbc4726f0ff5baceded8ac8b392a6587e9e7d17c68d3dca124effd36fed187f6a50ba2fc81ebfbaa53cd6dc389ce036ef3c76350e521576a9dc9c727b1163402d1d8d02edb49e8') {
        return Promise.reject(new Error('rsa bridge failed'))
    }
    return lx.utils.zlib.deflate(input).then(compressed => lx.utils.zlib.inflate(compressed)).then(roundtrip => {
        if (lx.utils.buffer.bufToString(roundtrip) !== 'contract') throw new Error('zlib bridge failed')
        return new Promise((resolve, reject) => {
            let endpoint = ''
            let options = { method: 'get', timeout: 2000 }
            if (info.musicInfo.id === 'cancel') endpoint = '/hang'
            if (info.musicInfo.id === 'form') {
                endpoint = '/form'
                options = { method: 'post', form: { alpha: 'one two', beta: '三' }, timeout: 2000 }
            }
            if (info.musicInfo.id === 'multipart') {
                endpoint = '/multipart'
                options = { method: 'post', formData: { field: 'value', binary: Buffer.from('raw-bytes') }, timeout: 2000 }
            }
            request('http://music.invalid' + endpoint, options, (error, response, body) => {
                if (error) return reject(error)
                if (!response || !body?.url || response.body !== body || !Buffer.isBuffer(response.raw)) return reject(new Error('bad response'))
                console.log('request complete')
                resolve(body.url)
            })
        })
    })
})
send(EVENT_NAMES.updateAlert, { log: 'compat update', updateUrl: 'https://example.invalid/update' })
send(EVENT_NAMES.inited, {
    status: true,
    sources: { kw: { type: 'music', actions: ['musicUrl'], qualitys: ['320k'] } }
})
)JS");
    QFile plugin(temp.filePath(QStringLiteral("request-source.js")));
    QVERIFY(plugin.open(QIODevice::WriteOnly | QIODevice::Text));
    const QByteArray scriptBytes = script.toUtf8();
    QCOMPARE(plugin.write(scriptBytes), scriptBytes.size());
    plugin.close();

    listenfree::sourcehost::SourceHostClient client(executable);
    QSignalSpy readySpy(&client, &listenfree::sourcehost::SourceHostClient::ready);
    QSignalSpy finishedSpy(&client, &listenfree::sourcehost::SourceHostClient::requestFinished);
    listenfree::sourcehost::SourceMessage lastMessage;
    connect(&client, &listenfree::sourcehost::SourceHostClient::messageReceived, &client,
            [&](const listenfree::sourcehost::SourceMessage& message) { lastMessage = message; });
    QVERIFY(client.start());
    QTRY_COMPARE_WITH_TIMEOUT(readySpy.count(), 1, 2000);
    listenfree::sourcehost::SourceMessage load;
    load.type = listenfree::sourcehost::MessageType::LoadPlugin;
    load.requestId = QStringLiteral("request-load");
    load.payload.insert(QStringLiteral("path"), plugin.fileName());
    load.payload.insert(QStringLiteral("proxy"),
                        QJsonObject{{QStringLiteral("host"), QStringLiteral("127.0.0.1")},
                                    {QStringLiteral("port"), server.serverPort()}});
    QVERIFY(client.request(load, 2000));
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 1000);
    QCOMPARE(finishedSpy.takeFirst().at(1).value<listenfree::sourcehost::SourceHostClient::RequestTerminal>(),
             listenfree::sourcehost::SourceHostClient::RequestTerminal::Succeeded);
    QCOMPARE(lastMessage.payload.value(QStringLiteral("updateAlert")).toObject()
                 .value(QStringLiteral("log")).toString(), QStringLiteral("compat update"));

    listenfree::sourcehost::SourceMessage resolve;
    resolve.type = listenfree::sourcehost::MessageType::ResolveMusicUrl;
    resolve.requestId = QStringLiteral("request-resolve");
    resolve.payload.insert(QStringLiteral("source"), QStringLiteral("kw"));
    resolve.payload.insert(QStringLiteral("type"), QStringLiteral("320k"));
    resolve.payload.insert(QStringLiteral("musicInfo"), QJsonObject{{QStringLiteral("id"), QStringLiteral("song-3")}});
    QVERIFY(client.request(resolve, 3000));
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 2500);
    QCOMPARE(finishedSpy.takeFirst().at(1).value<listenfree::sourcehost::SourceHostClient::RequestTerminal>(),
             listenfree::sourcehost::SourceHostClient::RequestTerminal::Succeeded);
    QCOMPARE(lastMessage.payload.value(QStringLiteral("data")).toObject().value(QStringLiteral("url")).toString(),
             QStringLiteral("https://media.invalid/from-request.mp3"));

    for (const auto& id : {QStringLiteral("form"), QStringLiteral("multipart")}) {
        listenfree::sourcehost::SourceMessage bridgeRequest = resolve;
        bridgeRequest.requestId = QStringLiteral("request-") + id;
        bridgeRequest.payload.insert(QStringLiteral("musicInfo"),
                                     QJsonObject{{QStringLiteral("id"), id}});
        QVERIFY(client.request(bridgeRequest, 3000));
        QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 2500);
        QCOMPARE(finishedSpy.takeFirst().at(1)
                     .value<listenfree::sourcehost::SourceHostClient::RequestTerminal>(),
                 listenfree::sourcehost::SourceHostClient::RequestTerminal::Succeeded);
    }
    QVERIFY(formRequestValid);
    QVERIFY(multipartRequestValid);
    QVERIFY(proxyRequestObserved);

    listenfree::sourcehost::SourceMessage cancelResolve = resolve;
    cancelResolve.requestId = QStringLiteral("request-cancel");
    cancelResolve.payload.insert(QStringLiteral("musicInfo"), QJsonObject{{QStringLiteral("id"), QStringLiteral("cancel")}});
    QVERIFY(client.request(cancelResolve, 3000));
    QTRY_VERIFY_WITH_TIMEOUT(hangingRequest, 1000);
    client.cancel("request-cancel");
    QTRY_COMPARE_WITH_TIMEOUT(finishedSpy.count(), 1, 1000);
    QCOMPARE(finishedSpy.takeFirst().at(1).value<listenfree::sourcehost::SourceHostClient::RequestTerminal>(),
             listenfree::sourcehost::SourceHostClient::RequestTerminal::Cancelled);
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
    QSignalSpy countChanged(&model, &listenfree::qmlbridge::TrackListModel::countChanged);
    listenfree::domain::Track track;
    track.id = listenfree::domain::TrackId("model-track");
    track.title = "Model Track";
    model.setTracks({track});
    QCOMPARE(model.rowCount(), 1);
    QCOMPARE(model.data(model.index(0, 0), listenfree::qmlbridge::TrackListModel::TitleRole).toString(),
             QStringLiteral("Model Track"));
    QCOMPARE(model.property("count").toInt(), 1);
    QCOMPARE(countChanged.count(), 1);
    QCOMPARE(model.get(0).value("title").toString(), QStringLiteral("Model Track"));
    QVERIFY(model.get(-1).isEmpty());
    QVERIFY(model.get(1).isEmpty());
    model.setTracks({});
    QCOMPARE(model.property("count").toInt(), 0);
    QCOMPARE(countChanged.count(), 2);
    QVERIFY(model.get(0).isEmpty());
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
