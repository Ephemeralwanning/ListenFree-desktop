#include "infrastructure/library/library_scanner.h"

#include <QDirIterator>
#include <QFileInfo>
#include <QtConcurrent/QtConcurrentRun>

#include <taglib/audioproperties.h>
#include <taglib/fileref.h>
#include <taglib/tag.h>

#include <system_error>

namespace listenfree::infrastructure::library {

LibraryScanner::LibraryScanner(QObject* parent) : QObject(parent) {
    connect(&watcher_, &QFutureWatcher<QVector<QString>>::finished, this, [this] {
        if (cancelled_ && cancelled_->load()) {
            emit finished();
            return;
        }
        emit tracksFound(watcher_.result());
        emit finished();
    });
}

LibraryScanner::~LibraryScanner() {
    cancel();
    watcher_.waitForFinished();
}

void LibraryScanner::start(const QStringList& roots, bool recursive) {
    cancel();
    // Do not replace the watcher future while the previous worker still owns
    // its cancellation token and result buffer. Cancellation is cooperative,
    // so waiting here makes repeated starts deterministic and leak-free.
    watcher_.waitForFinished();
    cancelled_ = std::make_shared<std::atomic_bool>(false);
    const auto token = cancelled_;
    watcher_.setFuture(QtConcurrent::run([roots, recursive, token] {
        QVector<QString> paths;
        const QStringList filters{QStringLiteral("*.mp3"), QStringLiteral("*.flac"), QStringLiteral("*.wav"),
                                  QStringLiteral("*.aac"), QStringLiteral("*.m4a"), QStringLiteral("*.ogg"),
                                  QStringLiteral("*.oga"), QStringLiteral("*.opus"), QStringLiteral("*.wma"),
                                  QStringLiteral("*.ape"), QStringLiteral("*.wv"), QStringLiteral("*.aiff"),
                                  QStringLiteral("*.aif"), QStringLiteral("*.tta"), QStringLiteral("*.mp4")};
        for (const QString& root : roots) {
            const auto flags = recursive ? QDirIterator::Subdirectories : QDirIterator::NoIteratorFlags;
            QDirIterator it(root, filters, QDir::Files, flags);
            while (it.hasNext()) {
                if (token->load()) return paths;
                paths.push_back(it.next());
            }
        }
        return paths;
    }));
}

void LibraryScanner::cancel() {
    if (cancelled_) cancelled_->store(true);
}

std::optional<domain::Track> BasicMetadataReader::read(const std::filesystem::path& path) {
    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    if (error || canonical.empty() || !std::filesystem::is_regular_file(canonical, error) || error) {
        return std::nullopt;
    }
    domain::Track track;
    track.id = domain::TrackId(canonical.string());
    track.title = canonical.stem().string();
    track.localPath = canonical.string();
    return track;
}

std::optional<domain::Track> TagLibMetadataReader::read(const std::filesystem::path& path) {
    auto track = fallback_.read(path);
    if (!track) return std::nullopt;

    std::error_code error;
    const auto canonical = std::filesystem::weakly_canonical(path, error);
    if (error) return track;

#ifdef Q_OS_WIN
    TagLib::FileRef file(canonical.c_str(), true, TagLib::AudioProperties::Fast);
#else
    TagLib::FileRef file(canonical.string().c_str(), true, TagLib::AudioProperties::Fast);
#endif
    if (file.isNull()) return track;

    if (const auto* tag = file.tag()) {
        const auto title = tag->title().to8Bit(true);
        if (!title.empty()) track->title = title;

        const auto artist = tag->artist().to8Bit(true);
        if (!artist.empty()) track->artists = {{artist, artist}};

        const auto album = tag->album().to8Bit(true);
        if (!album.empty()) track->album = domain::Album{album, album, std::nullopt};
    }

    if (const auto* properties = file.audioProperties()) {
        const auto milliseconds = properties->lengthInMilliseconds();
        if (milliseconds > 0) track->duration = std::chrono::milliseconds(milliseconds);
    }

    return track;
}

LocalLibraryScannerAdapter::LocalLibraryScannerAdapter(
    std::unique_ptr<application::IMetadataReader> metadataReader, QObject* parent)
    : QObject(parent), scanner_(this), metadataReader_(std::move(metadataReader)) {
    if (!metadataReader_) metadataReader_ = std::make_unique<TagLibMetadataReader>();
    connect(&scanner_, &LibraryScanner::tracksFound, this, [this](const QVector<QString>& paths) {
        for (const auto& path : paths) {
            if (cancelled_ && cancelled_()) {
                scanner_.cancel();
                break;
            }
            const auto track = metadataReader_->read(std::filesystem::path(path.toStdWString()));
            if (track && onTrack_) onTrack_(*track);
        }
    });
    connect(&scanner_, &LibraryScanner::failed, this, [this](const QString& message) {
        if (onError_) onError_(message.toStdString());
    });
}

LocalLibraryScannerAdapter::~LocalLibraryScannerAdapter() { cancel(); }

void LocalLibraryScannerAdapter::start(const application::ScanRequest& request,
                                       std::function<void(domain::Track)> onTrack,
                                       std::function<void(std::string)> onError,
                                       application::CancelCallback cancelled) {
    onTrack_ = std::move(onTrack);
    onError_ = std::move(onError);
    cancelled_ = std::move(cancelled);
    QStringList roots;
    for (const auto& root : request.roots) roots.push_back(QString::fromStdWString(root.wstring()));
    scanner_.start(roots, request.recursive);
}

void LocalLibraryScannerAdapter::cancel() {
    scanner_.cancel();
    onTrack_ = {};
    onError_ = {};
    cancelled_ = {};
}

} // namespace listenfree::infrastructure::library
