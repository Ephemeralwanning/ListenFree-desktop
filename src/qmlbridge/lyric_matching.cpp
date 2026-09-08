#include "qmlbridge/portable_session.h"
#include "online/platform_catalog.h"
#include "online/lyric_matching.h"
#include "online/lyric_sources.h"
#include <QDateTime>
#include "online/kuwo_lyrics.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <algorithm>
#include <QBuffer>
#include <QImageReader>
#include <QSaveFile>
#include <QFileInfo>
#include <QTimer>
#include <taglib/fileref.h>
#include <taglib/tpropertymap.h>
#include <QUuid>
#include <QtConcurrentRun>

namespace listenfree::qmlbridge {
void PortableSession::cancelMetadataMatch() {
    ++metadataArtworkGeneration_;
    if (metadataArtworkReply_) {
        auto reply = metadataArtworkReply_; metadataArtworkReply_ = nullptr; reply->abort();
    }
    metadataArtwork_.clear(); metadataArtworkIndex_ = -1; emit metadataArtworkChanged();
    if (metadataMatchReply_) {
        auto reply = metadataMatchReply_; metadataMatchReply_ = nullptr; reply->abort();
    }
    metadataMatchBusy_ = false;
    metadataCandidates_.clear(); metadataMatchError_.clear(); emit metadataMatchChanged();
}
void PortableSession::searchMetadataMatches(const QVariantMap& track, const QString& query, const QString& source) {
    cancelMetadataMatch();
    const auto term = (query.trimmed().isEmpty()
        ? track.value("title").toString() + " " + track.value("artist").toString() : query).trimmed();
    if (term.isEmpty() || (source != "wy" && source != "tx")) {
        metadataMatchError_ = QStringLiteral("请输入歌曲或艺术家，并选择搜索来源"); emit metadataMatchChanged(); return;
    }
    auto* reply = online::platformRequest(network_, source, "search", term);
    if (!reply) { metadataMatchError_ = QStringLiteral("信息来源不可用"); emit metadataMatchChanged(); return; }
    metadataMatchReply_ = reply; metadataMatchBusy_ = true; emit metadataMatchChanged();
    connect(reply, &QNetworkReply::finished, this, [this, reply, track, source] {
        reply->deleteLater(); if (metadataMatchReply_ != reply) return;
        metadataMatchReply_ = nullptr; metadataMatchBusy_ = false;
        if (reply->error() != QNetworkReply::NoError) metadataMatchError_ = QStringLiteral("搜索失败，请重试或切换来源");
        else {
            metadataCandidates_ = online::platformSongs(source, online::platformJson(reply->readAll()));
            for (auto& candidate : metadataCandidates_) {
                auto row = candidate.toMap(); row["score"] = online::lyricCandidateScore(track, row); candidate = row;
            }
            std::stable_sort(metadataCandidates_.begin(), metadataCandidates_.end(), [](const auto& a, const auto& b) {
                return a.toMap().value("score").toInt() > b.toMap().value("score").toInt();
            });
            if (metadataCandidates_.isEmpty()) metadataMatchError_ = QStringLiteral("没有找到结果，可简化关键词或切换来源");
        }
        emit metadataMatchChanged();
    });
}
QVariantMap PortableSession::metadataMatchValues(int index, const QStringList& fields) const {
    QVariantMap values;
    if (index < 0 || index >= metadataCandidates_.size()) return values;
    const auto candidate = metadataCandidates_[index].toMap();
    for (const auto& field : {QStringLiteral("title"), QStringLiteral("artist"), QStringLiteral("album")}) {
        const auto text = candidate.value(field).toString().trimmed();
        if (fields.contains(field) && !text.isEmpty()) values[field] = text;
    }
    if (fields.contains("artwork") && metadataArtworkIndex_ == index && !metadataArtwork_.value("url").toString().isEmpty())
        values["artwork"] = metadataArtwork_.value("url");
    return values;
}
void PortableSession::previewMetadataArtwork(int index) {
    const auto generation = ++metadataArtworkGeneration_;
    if (metadataArtworkReply_) {
        auto reply = metadataArtworkReply_; metadataArtworkReply_ = nullptr; reply->abort();
    }
    metadataArtworkIndex_ = index; metadataArtwork_.clear(); emit metadataArtworkChanged();
    if (index < 0 || index >= metadataCandidates_.size()) return;
    const auto candidate = metadataCandidates_[index].toMap();
    metadataArtwork_["busy"] = true; emit metadataArtworkChanged();
    const auto url = candidate.value("artwork").toUrl();
    if (!url.isEmpty()) { fetchMetadataArtwork(url, generation); return; }
    // NetEase search often omits the album picture; use the existing song detail request.
    if (candidate.value("source").toString() == "wy") {
        auto* reply = online::platformRequest(network_, "wy", "song", candidate.value("rid").toString());
        if (reply) {
            metadataArtworkReply_ = reply;
            connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
                reply->deleteLater(); if (generation != metadataArtworkGeneration_) return;
                metadataArtworkReply_ = nullptr;
                const auto rows = online::platformSongs("wy", online::platformJson(reply->readAll()));
                fetchMetadataArtwork(rows.isEmpty() ? QUrl{} : rows.first().toMap().value("artwork").toUrl(), generation);
            });
            return;
        }
    }
    fetchMetadataArtwork({}, generation);
}
void PortableSession::fetchMetadataArtwork(const QUrl& url, quint64 generation) {
    if (generation != metadataArtworkGeneration_) return;
    if (url.scheme() != "http" && url.scheme() != "https") {
        metadataArtwork_ = {{"error", "此版本未提供封面，保留当前封面"}}; emit metadataArtworkChanged(); return;
    }
    QNetworkRequest request(url); request.setTransferTimeout(12000);
    auto* reply = network_.get(request); metadataArtworkReply_ = reply;
    connect(reply, &QNetworkReply::downloadProgress, this, [reply](qint64 received, qint64 total) {
        if (qMax(received, total) > 12 * 1024 * 1024) reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
        reply->deleteLater(); if (generation != metadataArtworkGeneration_) return;
        metadataArtworkReply_ = nullptr;
        const auto bytes = reply->readAll();
        if (reply->error() != QNetworkReply::NoError || bytes.isEmpty()) {
            metadataArtwork_ = {{"error", "封面加载失败，重新选择可重试"}}; emit metadataArtworkChanged(); return;
        }
        auto* watcher = new QFutureWatcher<QVariantMap>(this);
        connect(watcher, &QFutureWatcher<QVariantMap>::finished, this, [this, watcher, generation] {
            const auto result = watcher->result(); watcher->deleteLater();
            if (generation != metadataArtworkGeneration_) return;
            metadataArtwork_ = result;
            const auto path = result.value("url").toUrl().toLocalFile();
            if (!path.isEmpty()) metadataArtworkFiles_.insert(path);
            emit metadataArtworkChanged();
        });
        watcher->setFuture(QtConcurrent::run([bytes, directory = metadataArtworkDirectory_] {
            QBuffer buffer; buffer.setData(bytes); buffer.open(QIODevice::ReadOnly);
            QImageReader reader(&buffer); reader.setAutoTransform(true);
            const auto size = reader.size();
            const QVariantMap failure{{"error", "封面图片无法读取，保留当前封面"}};
            if (!size.isValid() || qint64(size.width()) * size.height() > 64000000 || !directory->isValid()) return failure;
            if (size.width() > 1600 || size.height() > 1600) reader.setScaledSize(size.scaled(1600,1600,Qt::KeepAspectRatio));
            const auto image = reader.read();
            const auto path = directory->filePath(QUuid::createUuid().toString(QUuid::WithoutBraces) + ".jpg");
            if (image.isNull() || !image.save(path, "JPEG", 94)) return failure;
            return QVariantMap{{"url", QUrl::fromLocalFile(path).toString()}, {"width", image.width()}, {"height", image.height()}};
        }));
    });
}
namespace {
constexpr int verifiedVersionsPerSource=3;
QString lyricKey(const QVariantMap& track) {
    return "lyrics.override."+QString::fromLatin1((track.value("localPath").toString().isEmpty()
        ? track.value("source").toString()+":"+track.value("rid",track.value("trackId")).toString()
        : track.value("localPath").toString()).toUtf8().toBase64(QByteArray::Base64UrlEncoding));
}
}
void PortableSession::cancelLyricMatch() {
    ++lyricMatchGeneration_;lyricVerifying_=false;
    const auto searches=lyricSearchReplies_;lyricSearchReplies_.clear();
    for(auto reply:searches)if(reply)reply->abort();
    const auto probes=lyricProbeReplies_;lyricProbeReplies_.clear();
    for(auto reply:probes)if(reply)reply->abort();
    lyricMatchBusy_=false;emit lyricMatchChanged();
}
QVariantMap PortableSession::lyricMatchSeed(const QVariantMap& track) const {
    auto seed=readTrackTags(track);
    const auto clean=[](QString text) {
        text=text.simplified();
        const QStringList placeholders{"未知歌曲","未知标题","未知艺术家","未知专辑","Unknown","Unknown Title","Unknown Artist","Unknown Album"};
        return placeholders.contains(text,Qt::CaseInsensitive)?QString{}:text;
    };
    for(const auto* field:{"title","artist","album"})seed[field]=clean(seed.value(field).toString());
    const auto title=seed.value("title").toString();
    const auto filename=QFileInfo(track.value("localPath").toString()).completeBaseName().trimmed();
    // A missing title tag must not leave the search field blank or search only
    // for a placeholder artist. Keep the actual filename, without its suffix.
    if(title.isEmpty())seed["title"]=filename;
    seed["queryFromFilename"]=title.isEmpty();
    seed["query"]=title.isEmpty()?filename:(title+" "+seed.value("artist").toString()).trimmed();
    return seed;
}
void PortableSession::searchLyricMatches(const QVariantMap& track,const QString& query,const QString& source) {
    cancelLyricMatch();lyricMatchTrack_=query.trimmed().isEmpty()?lyricMatchSeed(track):track;
    lyricCandidates_.clear();lyricPreview_.clear();lyricMatchError_.clear();lyricSearchRows_.clear();lyricSearchFailures_.clear();
    lyricPreviewLines_.clear();lyricProbeCache_.clear();lyricProbePool_.clear();emit lyricPreviewChanged();
    const auto term=query.trimmed().isEmpty()?lyricMatchTrack_.value("query").toString():query.trimmed();
    lyricMatchQuery_=term;
    if(term.isEmpty()){lyricMatchError_="请输入歌曲或艺术家";emit lyricMatchChanged();return;}
    const QStringList sources=source=="all"?QStringList{"wy","tx","kg","kw","lrclib","amll"}:QStringList{source};
    for(const auto& provider:sources) {
        if(lyricRetryAfter_.value(provider)>QDateTime::currentMSecsSinceEpoch()) {
            lyricSearchFailures_.append(online::lyricSourceName(provider));continue;
        }
        if(!requestLyricSearch(provider,term))lyricSearchFailures_.append(online::lyricSourceName(provider));
    }
    lyricMatchBusy_=!lyricSearchReplies_.isEmpty();
    if(!lyricMatchBusy_)finishLyricSearch();
    else emit lyricMatchChanged();
}
bool PortableSession::requestLyricSearch(const QString& provider,const QString& term,bool fallback) {
    auto* reply=online::lyricSearchRequest(network_,provider,lyricMatchTrack_,term);
    if(!reply)return false;
    lyricSearchReplies_[provider]=reply;
    connect(reply,&QNetworkReply::finished,this,[this,reply,provider,term,fallback] {
            reply->deleteLater();if(lyricSearchReplies_.value(provider)!=reply)return;
            lyricSearchReplies_.remove(provider);
            if(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()==429) {
                const auto seconds=qMax(1,reply->rawHeader("Retry-After").toInt());
                lyricRetryAfter_[provider]=QDateTime::currentMSecsSinceEpoch()+qint64(seconds)*1000;
                lyricSearchFailures_.append(online::lyricSourceName(provider));
            } else if(reply->error()!=QNetworkReply::NoError)lyricSearchFailures_.append(online::lyricSourceName(provider));
            else {
                const auto rows=online::lyricSearchResults(provider,reply->readAll());
                lyricSearchRows_[provider].append(rows);
                // Some catalogs treat artist + title as a loose OR query. A
                // title-only fallback also recovers transient empty responses.
                auto simpler=term;
                const auto artist=lyricMatchTrack_.value("artist").toString().trimmed();
                if(!artist.isEmpty() && simpler.endsWith(artist,Qt::CaseInsensitive))simpler.chop(artist.size());
                simpler=simpler.trimmed();
                const bool closeTitle=std::any_of(rows.cbegin(),rows.cend(),[&](const auto& value) {
                    return online::lyricTitleSimilarity(simpler,value.toMap().value("title").toString())>=.85;
                });
                if(!fallback && !closeTitle && !simpler.isEmpty() && simpler!=term && requestLyricSearch(provider,simpler,true))return;
            }
            if(lyricSearchReplies_.isEmpty())finishLyricSearch();
        });
    return true;
}
void PortableSession::finishLyricSearch() {
    const auto normal=online::normalizedLyricTitle(lyricMatchTrack_.value("title").toString()+" "+lyricMatchTrack_.value("artist").toString());
    const auto query=online::normalizedLyricTitle(lyricMatchQuery_);
    const bool manual=lyricMatchTrack_.value("queryFromFilename").toBool()
        || (query!=normal && query!=online::normalizedLyricTitle(lyricMatchTrack_.value("query").toString()));
    QVariantList ranked;
    for(const auto& provider:QStringList{"wy","tx","kg","kw","lrclib","amll"})for(const auto& item:lyricSearchRows_.value(provider)) {
        auto row=item.toMap();
        const auto title=row.value("title").toString();
        row["score"]=manual?qRound(100*qMax(online::lyricTextSimilarity(query,title),online::lyricTextSimilarity(query,title+" "+row.value("artist").toString())))
            :online::lyricCandidateScore(lyricMatchTrack_,row);
        ranked.append(row);
    }
    lyricSearchRows_.clear();
    std::stable_sort(ranked.begin(),ranked.end(),[](const auto& a,const auto& b){return a.toMap().value("score").toInt()>b.toMap().value("score").toInt();});
    const int best=ranked.isEmpty()?0:ranked.first().toMap().value("score").toInt();
    QSet<QString> identities;
    for(const auto& item:ranked) {
        const auto row=item.toMap();
        const auto title=row.value("title").toString();
        const auto left=lyricMatchTrack_.value("durationMs").toLongLong(),right=row.value("durationMs").toLongLong();
        if(row.value("score").toInt()<qMax(manual?0:60,best-15))continue;
        if(!manual && online::lyricTitleSimilarity(lyricMatchTrack_.value("title").toString(),title)<.55)continue;
        if(manual && qMax(online::lyricTextSimilarity(query,title),online::lyricTextSimilarity(query,title+" "+row.value("artist").toString()))<.45)continue;
        if(!manual && left>0 && right>0 && qAbs(left-right)>15000)continue;
        const auto identity=row.value("lyricSource").toString()+":"+row.value("rid").toString();
        if(identities.contains(identity))continue;
        identities.insert(identity);lyricProbePool_.append(row);
    }
    lyricVerifying_=true;lyricMatchBusy_=true;
    for(int i=0;i<lyricProbePool_.size();++i) {
        auto row=lyricProbePool_[i].toMap();
        if(row.contains("lyricsText")){lyricProbeCache_[i]=row.take("lyricsText").toString();lyricProbePool_[i]=row;}
    }
    // LDDC also bounds its verification phase. Preserve verified results when a
    // provider stalls; never publish an unverified entry to fill the list.
    const auto generation=lyricMatchGeneration_;
    QTimer::singleShot(20000,this,[this,generation] {
        if(generation!=lyricMatchGeneration_ || !lyricVerifying_)return;
        const auto replies=lyricProbeReplies_;lyricProbeReplies_.clear();
        for(auto reply:replies)if(reply)reply->abort();
        finishLyricVerification();
    });
    emit lyricMatchChanged();pumpLyricCandidates();
}
void PortableSession::finishLyricVerification() {
    if(!lyricVerifying_)return;
    lyricVerifying_=false;
    QVariantList verified;QHash<int,QString> cache;QSet<QString> duplicates;QHash<QString,int> counts;
    for(int i=0;i<lyricProbePool_.size();++i) {
        const auto text=lyricProbeCache_.value(i);
        if(!online::hasUsableMatchedLyrics(text))continue;
        auto row=lyricProbePool_[i].toMap();
        const auto source=row.value("lyricSource").toString();
        if(counts.value(source)>=verifiedVersionsPerSource)continue;
        const auto identity=source+QChar(0x1f)+online::matchedLyricIdentity(text);
        if(duplicates.contains(identity))continue;
        duplicates.insert(identity);++counts[source];row["features"]=online::matchedLyricFeatures(text);
        cache[int(verified.size())]=text;verified.append(row);
    }
    lyricCandidates_=verified;lyricProbeCache_=cache;lyricProbePool_.clear();lyricMatchBusy_=false;
    if(lyricCandidates_.isEmpty())lyricMatchError_=lyricSearchFailures_.isEmpty()?"未找到可用歌词，试试只搜索歌名或调整关键词":"暂未获取到可用歌词，部分来源请求失败，请稍后重试";
    else if(!lyricSearchFailures_.isEmpty())lyricMatchError_="部分来源暂不可用，已保留验证成功的歌词";
    emit lyricMatchChanged();
}
void PortableSession::previewLyricMatch(int index) {
    if(lyricMatchBusy_ || index<0 || index>=lyricCandidates_.size())return;
    selectCachedLyric(index);emit lyricMatchChanged();
}
void PortableSession::selectCachedLyric(int index) {
    lyricPreview_=lyricProbeCache_.value(index);lyricPreviewLines_=online::parseTimedLyrics(lyricPreview_);
    if(lyricPreviewLines_.isEmpty()&&!lyricPreview_.trimmed().isEmpty())
        for(const auto& line:lyricPreview_.split('\n'))if(!line.trimmed().isEmpty())lyricPreviewLines_.append(QVariantMap{{"text",line}});
    lyricMatchError_.clear();emit lyricPreviewChanged();
}
void PortableSession::pumpLyricCandidates() {
    if(!lyricVerifying_)return;
    QSet<QString> active;
    QHash<QString,QSet<QString>> verified;
    for(auto it=lyricProbeCache_.cbegin();it!=lyricProbeCache_.cend();++it)
        if(online::hasUsableMatchedLyrics(it.value()))verified[lyricProbePool_[it.key()].toMap().value("lyricSource").toString()].insert(online::matchedLyricIdentity(it.value()));
    for(auto it=lyricProbeReplies_.cbegin();it!=lyricProbeReplies_.cend();++it)active.insert(lyricProbePool_[it.key()].toMap().value("lyricSource").toString());
    // One fetch per provider keeps slower sources independent and avoids bursts
    // of requests to one service for reissues of the same recording.
    for(int i=0;i<lyricProbePool_.size() && lyricProbeReplies_.size()<6;++i) {
        if(lyricProbeCache_.contains(i)||lyricProbeReplies_.contains(i))continue;
        const auto source=lyricProbePool_[i].toMap().value("lyricSource").toString();
        if(verified.value(source).size()>=verifiedVersionsPerSource || lyricRetryAfter_.value(source)>QDateTime::currentMSecsSinceEpoch()){lyricProbeCache_[i]={};continue;}
        if(active.contains(source))continue;
        requestLyricCandidate(i);
        if(lyricProbeReplies_.contains(i))active.insert(source);
    }
    if(lyricProbeReplies_.isEmpty() && lyricProbeCache_.size()==lyricProbePool_.size())finishLyricVerification();
}
void PortableSession::requestLyricCandidate(int index) {
    const auto source=lyricProbePool_[index].toMap().value("lyricSource").toString();
    auto* reply=online::lyricFetchRequest(network_,source,lyricProbePool_[index].toMap());
    if(!reply){lyricProbeCache_[index]={};return;}
    lyricProbeReplies_[index]=reply;
    connect(reply,&QNetworkReply::finished,this,[this,reply,index,source] {
        reply->deleteLater();if(lyricProbeReplies_.value(index)!=reply)return;
        lyricProbeReplies_.remove(index);
        const auto status=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if(status==429)lyricRetryAfter_[source]=QDateTime::currentMSecsSinceEpoch()+qint64(qMax(1,reply->rawHeader("Retry-After").toInt()))*1000;
        if(reply->error()!=QNetworkReply::NoError) {
            lyricProbeCache_[index]={};
            if(status!=404 && !lyricSearchFailures_.contains(source))lyricSearchFailures_.append(source);
        } else lyricProbeCache_[index]=online::lyricResponse(source,reply->readAll());
        pumpLyricCandidates();
    });
}
bool PortableSession::applyLyricMatch(const QVariantMap& track,const QString& lyrics) {
    const auto path=track.value("localPath").toString();
    if(!path.isEmpty()) {
        const QFileInfo media(path);
        if(!media.isFile()){emit notice("本地文件不存在");return false;}
        const auto previous=pendingEmbeddedLyrics_;
        pendingEmbeddedLyrics_[media.absoluteFilePath()]=lyrics;
        if(!persistEmbeddedLyrics()) { pendingEmbeddedLyrics_=previous;emit notice("歌词保存任务创建失败");return false; }
        embeddedLyricFailures_.remove(media.absoluteFilePath());
        if(QFileInfo(currentTrack().value("localPath").toString())==media)loadLyrics(path);
        pumpEmbeddedLyrics();
        return true;
    }
    if(!database_.setSetting(lyricKey(track),lyrics)){emit notice("歌词保存失败");return false;}
    if(lyricKey(currentTrack())==lyricKey(track)) {
        if(lyricsReply_){auto reply=lyricsReply_;lyricsReply_=nullptr;reply->abort();}
        lyrics_=online::parseTimedLyrics(lyrics);emit lyricsChanged();
    }
    return true;
}

void PortableSession::initializeEmbeddedLyrics() {
    pendingEmbeddedLyrics_=QJsonDocument::fromJson(QByteArray::fromStdString(database_.getSetting("lyrics.pendingEmbedded").value_or("{}"))).object().toVariantMap();
    connect(&embeddedLyricWrite_,&QFutureWatcher<QString>::finished,this,&PortableSession::finishEmbeddedLyrics);
    QTimer::singleShot(0,this,&PortableSession::pumpEmbeddedLyrics);
}
bool PortableSession::persistEmbeddedLyrics() {
    return database_.setSetting("lyrics.pendingEmbedded",QString::fromUtf8(QJsonDocument(QJsonObject::fromVariantMap(pendingEmbeddedLyrics_)).toJson(QJsonDocument::Compact)));
}
void PortableSession::pumpEmbeddedLyrics() {
    if(!embeddedLyricWritePath_.isEmpty())return;
    for(auto it=pendingEmbeddedLyrics_.cbegin();it!=pendingEmbeddedLyrics_.cend();++it) {
        const auto mixIndex=mixTargetIndex();
        if (mixIndex>=0 && QFileInfo(it.key())==QFileInfo(entries_.value(navigation_.track(mixIndex)).value("localPath").toString())) continue;
        if(embeddedLyricFailures_.contains(it.key()) || (!decoderLocalPath_.isEmpty() && QFileInfo(it.key())==QFileInfo(decoderLocalPath_)))continue;
        embeddedLyricWriteTags_=readTrackTags({{"localPath",it.key()}});
        embeddedLyricWritePath_=it.key();embeddedLyricWriteText_=it->toString();
        embeddedLyricWrite_.setFuture(QtConcurrent::run([path=embeddedLyricWritePath_,text=embeddedLyricWriteText_]() -> QString {
            // This path is released by the decoder; new playback waits for this job.
            TagLib::FileRef file(path.toStdWString().c_str(),false);
            if(file.isNull())return QStringLiteral("无法读取音频标签");
            auto properties=file.file()->properties();
            properties.replace("LYRICS",TagLib::StringList(TagLib::String(text.toStdString(),TagLib::String::UTF8)));
            if(file.file()->setProperties(properties).contains("LYRICS"))return QStringLiteral("文件不支持内嵌歌词");
            if(!file.save())return QStringLiteral("内嵌歌词保存失败，请检查文件权限");
            const QFileInfo info(path);const auto sidecar=info.absolutePath()+"/"+info.completeBaseName()+".lrc";
            if(QFileInfo::exists(sidecar)) {
                QSaveFile output(sidecar);const auto bytes=text.toUtf8();
                if(!output.open(QIODevice::WriteOnly)||output.write(bytes)!=bytes.size()||!output.commit())return QStringLiteral("已有 LRC 同步失败");
            }
            return {};
        }));
        return;
    }
}
void PortableSession::finishEmbeddedLyrics() {
    if(embeddedLyricWritePath_.isEmpty() || !embeddedLyricWrite_.isFinished())return;
    const auto path=std::exchange(embeddedLyricWritePath_,{});
    const auto text=std::exchange(embeddedLyricWriteText_,{});
    const auto error=embeddedLyricWrite_.result();
    embeddedLyricWriteTags_.clear();
    if(error.isEmpty()) {
        if(pendingEmbeddedLyrics_.value(path).toString()==text)pendingEmbeddedLyrics_.remove(path);
        persistEmbeddedLyrics();
    } else {
        embeddedLyricFailures_.insert(path);
        emit notice(error+QStringLiteral("；歌词已挂载，保存任务将在下次启动时重试"));
    }
    if(!stopped_ && QFileInfo(currentTrack().value("localPath").toString())==QFileInfo(path)) {
        if(loading_&&decoderLocalPath_.isEmpty())beginCurrent();
        else loadLyrics(path);
    }
    pumpEmbeddedLyrics();
}
}
