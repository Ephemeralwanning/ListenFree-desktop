#include "infrastructure/library/library_scanner.h"

#include <QDirIterator>
#include <QtConcurrent/QtConcurrentRun>

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

void LibraryScanner::start(const QStringList& roots) {
    cancel();
    cancelled_ = std::make_shared<std::atomic_bool>(false);
    const auto token = cancelled_;
    watcher_.setFuture(QtConcurrent::run([roots, token] {
        QVector<QString> paths;
        const QStringList filters{QStringLiteral("*.mp3"), QStringLiteral("*.flac"), QStringLiteral("*.wav"),
                                  QStringLiteral("*.aac"), QStringLiteral("*.m4a"), QStringLiteral("*.ogg"),
                                  QStringLiteral("*.oga"), QStringLiteral("*.opus"), QStringLiteral("*.wma"),
                                  QStringLiteral("*.ape"), QStringLiteral("*.wv"), QStringLiteral("*.aiff"),
                                  QStringLiteral("*.aif"), QStringLiteral("*.tta"), QStringLiteral("*.mp4")};
        for (const QString& root : roots) {
            QDirIterator it(root, filters, QDir::Files, QDirIterator::Subdirectories);
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

} // namespace listenfree::infrastructure::library
