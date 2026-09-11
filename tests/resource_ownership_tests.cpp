#include <QApplication>
#include <QTest>
#include <QSignalSpy>
#include <QQuickWindow>
#include <QSGTexture>
#include <QRunnable>
#include <QTcpServer>
#include <QTcpSocket>
#include <QBuffer>
#include <QRandomGenerator>
#include <QQmlEngine>
#include <QQmlContext>
#include <QQmlComponent>
#include <QQuickItem>
#include <QQuickItemGrabResult>
#include <QTemporaryDir>
#include <QDir>
#include <QFile>
#include "qmlbridge/artwork_texture_factory.h"
#include "qmlbridge/remote_artwork_provider.h"
#include "qmlbridge/list_models.h"
#include "media/artwork_video.h"

class ResourceOwnershipTests : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void textureRecreatesExactPixels() {
        QImage original(512,384,QImage::Format_ARGB32_Premultiplied);
        for(int y=0;y<original.height();++y) for(int x=0;x<original.width();++x)
            original.setPixel(x,y,qRgba(x%128,y%128,(x+y)%128,160));
        original.setColorSpace(QColorSpace::SRgb); original.setDevicePixelRatio(1.5);
        const auto initialBitmap=ArtworkTextureFactory::liveBitmapBytes.load();
        const auto initialRecovery=ArtworkTextureFactory::liveRecoveryBytes.load();
        {
            ArtworkTextureFactory factory(original);
            QVERIFY(factory.recoveryBytes()>0); QCOMPARE(factory.image(),original);
            QQuickWindow window; window.resize(64,64); window.show();
            QVERIFY(QTest::qWaitForWindowExposed(&window));
            for(int attempt=0;attempt<2;++attempt) {
                std::atomic_int created{0};
                window.scheduleRenderJob(QRunnable::create([&] {
                    auto* texture=factory.createTexture(&window);
                    created.store(texture?1:-1); delete texture;
                }),QQuickWindow::BeforeRenderingStage);
                window.update(); QTRY_COMPARE_WITH_TIMEOUT(created.load(),1,5000);
                QCOMPARE(factory.retainedBitmapBytes(),0);
                const auto restored=factory.image();
                QCOMPARE(restored,original); QCOMPARE(restored.colorSpace(),original.colorSpace());
                QCOMPARE(restored.devicePixelRatio(),original.devicePixelRatio());
            }
            qInfo("artwork raw=%lld recovery=%lld",qint64(original.sizeInBytes()),qint64(factory.recoveryBytes()));
        }
        QCOMPARE(ArtworkTextureFactory::liveBitmapBytes.load(),initialBitmap);
        QCOMPARE(ArtworkTextureFactory::liveRecoveryBytes.load(),initialRecovery);
    }
    void smallAndIncompressibleTextures() {
        QImage tiny(32,24,QImage::Format_RGB32);tiny.fill(Qt::green);
        ArtworkTextureFactory small(tiny);QCOMPARE(small.recoveryBytes(),0);QCOMPARE(small.image(),tiny);
        QImage noise(512,384,QImage::Format_ARGB32_Premultiplied);
        auto* pixels=reinterpret_cast<quint32*>(noise.bits());
        QRandomGenerator random(43);random.fillRange(pixels,noise.sizeInBytes()/4);
        ArtworkTextureFactory large(noise);QCOMPARE(large.recoveryBytes(),0);QCOMPARE(large.image(),noise);
    }
    void sharedRowsKeepRolesAndCopyOnWrite() {
        using namespace listenfree::qmlbridge;
        QVariantList rows{QVariantMap{{"trackId","a"},{"title","曲目"},{"artist","歌手"},{"album","专辑"},
            {"durationMs",qint64(123456)},{"duration","2:03"},{"localPath","C:/a.mp3"},{"artwork","image://covers/a"}}};
        TrackListModel model; QueueModel queue;
        model.setRows(rows); queue.setRows(rows);
        QCOMPARE(model.get(0),queue.get(0)); QCOMPARE(model.get(0).value("duration").toLongLong(),123456);
        auto changed=rows[0].toMap();changed["title"]="更改";rows[0]=changed;
        QCOMPARE(model.get(0).value("title").toString(),QString("曲目"));
        model.setRows(rows);QCOMPARE(model.get(0).value("title").toString(),QString("更改"));
        QCOMPARE(queue.get(0).value("title").toString(),QString("曲目"));
        model.setTracks({});QCOMPARE(model.rowCount(),0);QVERIFY(model.get(0).isEmpty());
        model.setRows(rows);QCOMPARE(model.rowCount(),1);model.setRows({});QCOMPARE(model.rowCount(),0);
    }
    void remoteDecodeErrorAndCancellation() {
        QTcpServer server; QVERIFY(server.listen(QHostAddress::LocalHost));
        QImage input(120,80,QImage::Format_RGB32);input.fill(Qt::blue);
        QByteArray png;QBuffer buffer(&png);buffer.open(QIODevice::WriteOnly);input.save(&buffer,"PNG");
        connect(&server,&QTcpServer::newConnection,&server,[&] {
            while(auto* socket=server.nextPendingConnection()) {
                connect(socket,&QTcpSocket::readyRead,socket,[&,socket] {
                    const auto request=socket->readAll();
                    if(request.contains("/wait"))return;
                    const auto body=request.contains("/bad")?QByteArray("invalid"):png;
                    socket->write("HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: "+QByteArray::number(body.size())+"\r\n\r\n"+body);
                    socket->disconnectFromHost();
                });
            }
        });
        RemoteArtworkProvider provider;
        const auto request=[&](const QString& path) {
            return std::unique_ptr<QQuickImageResponse>(provider.requestImageResponse(
                QString::fromLatin1(QUrl::toPercentEncoding(QString("http://127.0.0.1:%1/%2").arg(server.serverPort()).arg(path))),QSize(60,60)));
        };
        auto good=request("image");QSignalSpy done(good.get(),&QQuickImageResponse::finished);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(),1,5000);QVERIFY2(good->errorString().isEmpty(),qPrintable(good->errorString()));
        std::unique_ptr<QQuickTextureFactory> texture(good->textureFactory());
        QCOMPARE(texture->textureSize(),QSize(90,60));QCOMPARE(texture->image().pixelColor(0,0),QColor(Qt::blue));
        {
            QQmlEngine engine; engine.addImageProvider("artwork",new RemoteArtworkProvider);
            QQmlComponent component(&engine);
            const auto url=QString("http://127.0.0.1:%1/image").arg(server.serverPort());
            component.setData("import QtQuick\nImage { source: 'image://artwork/"+QUrl::toPercentEncoding(url)
                +"'; sourceSize: Qt.size(60,60); fillMode: Image.PreserveAspectCrop }",QUrl());
            std::unique_ptr<QObject> image(component.create());QVERIFY2(image,qPrintable(component.errorString()));
            QTRY_COMPARE_WITH_TIMEOUT(image->property("status").toInt(),1,5000);
            const auto width=image->property("implicitWidth").toReal(), height=image->property("implicitHeight").toReal();
            QVERIFY(width>0 && height>0);
            // Qt multiplies sourceSize by the window's DPR before dispatching
            // provider requests, then exposes logical implicit dimensions.
            QCOMPARE(width/height,qreal(1.5));
        }
        auto bad=request("bad");QSignalSpy failed(bad.get(),&QQuickImageResponse::finished);
        QTRY_COMPARE_WITH_TIMEOUT(failed.size(),1,5000);QVERIFY(!bad->errorString().isEmpty());
        auto pending=request("wait");QSignalSpy cancelled(pending.get(),&QQuickImageResponse::finished);
        QTest::qWait(100);pending->cancel();QTRY_COMPARE_WITH_TIMEOUT(cancelled.size(),1,2000);
        QVERIFY(!pending->errorString().isEmpty());
    }
    void concurrentLargeArtworkKeepsPixelsAndCancels() {
        QImage original(2048,1536,QImage::Format_RGB32);
        for(int y=0;y<original.height();++y) for(int x=0;x<original.width();++x)
            original.setPixel(x,y,qRgb(x%256,y%256,(x+y)%256));
        QByteArray png;QBuffer encoded(&png);encoded.open(QIODevice::WriteOnly);QVERIFY(original.save(&encoded,"PNG"));
        QBuffer input(&png);input.open(QIODevice::ReadOnly);QImageReader reader(&input);
        reader.setScaledSize({1024,768});const auto expected=reader.read().convertToFormat(QImage::Format_RGB32);
        QTcpServer server;QVERIFY(server.listen(QHostAddress::LocalHost));
        connect(&server,&QTcpServer::newConnection,this,[&] {
            while(server.hasPendingConnections()) {
                auto* socket=server.nextPendingConnection();
                connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
                connect(socket,&QTcpSocket::readyRead,socket,[&,socket] {
                    socket->readAll();if(socket->property("sent").toBool())return;socket->setProperty("sent",true);
                    socket->write("HTTP/1.1 200 OK\r\nContent-Type: image/png\r\nContent-Length: "+QByteArray::number(png.size())+"\r\nConnection: close\r\n\r\n");
                    socket->write(png);socket->disconnectFromHost();
                });
            }
        });
        RemoteArtworkProvider provider;
        std::vector<std::unique_ptr<QQuickImageResponse>> responses;
        int finished=0;
        for(int i=0;i<4;++i) {
            auto* response=provider.requestImageResponse(QString::fromLatin1(QUrl::toPercentEncoding(
                QString("http://127.0.0.1:%1/large/%2").arg(server.serverPort()).arg(i))),{1024,768});
            connect(response,&QQuickImageResponse::finished,this,[&]{++finished;});
            responses.emplace_back(response);
        }
        responses.back()->cancel();
        QTRY_COMPARE_WITH_TIMEOUT(finished,4,12000);
        for(int i=0;i<3;++i) {
            QVERIFY2(responses[i]->errorString().isEmpty(),qPrintable(responses[i]->errorString()));
            std::unique_ptr<QQuickTextureFactory> factory(responses[i]->textureFactory());
            QVERIFY(factory);QCOMPARE(factory->image(),expected);
        }
        QVERIFY(!responses.back()->errorString().isEmpty());
    }
    void artworkVideoLoopPauseAndRelease() {
        const QString path=qEnvironmentVariable("LISTENFREE_TEST_COVER_VIDEO",QStringLiteral(LISTENFREE_TEST_SOURCE_DIR "/tests/fixtures/artwork-red-blue.mp4"));
        QVERIFY2(!path.isEmpty(),"Set LISTENFREE_TEST_COVER_VIDEO to a short H.264 fixture");
        listenfree::media::ArtworkVideo player;QVideoSink sink;
        player.setVideoSink(&sink);QSignalSpy frames(&sink,&QVideoSink::videoFrameChanged);
        QSignalSpy loops(&player,&listenfree::media::ArtworkVideo::looped);
        player.setPlaying(false);player.setSource(QUrl::fromLocalFile(path));
        QTRY_VERIFY_WITH_TIMEOUT(player.ready(),8000);QVERIFY(player.bounded());
        const auto frame=sink.videoFrame();QVERIFY(frame.isValid());
        QTest::qWait(200);QCOMPARE(sink.videoFrame().startTime(),frame.startTime());
        QVERIFY(player.queuedFrames()<=3);
        player.setPlaying(true);QTRY_VERIFY_WITH_TIMEOUT(loops.size()>=2,12000);
        QVERIFY(player.queuedFrames()<=3);player.setPlaying(false);
        QTest::qWait(60);const auto count=frames.size();QTest::qWait(200);QCOMPARE(frames.size(),count);
        player.setSource({});QVERIFY(!player.ready());QVERIFY(!sink.videoFrame().isValid());QCOMPARE(player.queuedFrames(),0);
        player.setSource(QUrl::fromLocalFile(path));QTRY_VERIFY_WITH_TIMEOUT(player.ready(),8000);
    }
    void artworkVideoQtFallbackAndPoster() {
        const auto path=qEnvironmentVariable("LISTENFREE_TEST_FALLBACK_VIDEO",QStringLiteral(LISTENFREE_TEST_SOURCE_DIR "/tests/fixtures/artwork-mpeg4.mp4"));
        QVERIFY(!path.isEmpty());
        listenfree::media::ArtworkVideo player; QVideoSink sink;player.setVideoSink(&sink);
        player.setPlaying(false);player.setSource(QUrl::fromLocalFile(path));
        QTRY_VERIFY_WITH_TIMEOUT(!player.bounded(),5000);
        QTRY_VERIFY_WITH_TIMEOUT(player.ready(),8000);
        QCOMPARE(sink.videoFrame().size(),QSize(160,90));
        QTest::qWait(100);const auto position=sink.videoFrame().startTime();
        QTest::qWait(200);QCOMPARE(sink.videoFrame().startTime(),position);
        player.setSource(QUrl::fromLocalFile(path+".missing"));
        QTRY_VERIFY_WITH_TIMEOUT(!player.bounded(),5000);QVERIFY(!player.ready());
        player.setSource({});QVERIFY(!sink.videoFrame().isValid());
    }
    void qmlCoverUsesNativeMovieAndFreezesBackground() {
        QTemporaryDir temporary;QVERIFY(temporary.isValid());
        const auto fixture=temporary.filePath("components");QVERIFY(QDir().mkpath(fixture));
        for(const auto* name:{"CoverArt","AppTheme","IconGlyph"})
            QVERIFY(QFile::copy(QStringLiteral(LISTENFREE_TEST_SOURCE_DIR "/music_player_desktop/components/")+name+".qml",fixture+"/"+name+".qml"));
        QFile module(fixture+"/qmldir");QVERIFY(module.open(QIODevice::WriteOnly));
        module.write("singleton AppTheme 1.0 AppTheme.qml\nCoverArt 1.0 CoverArt.qml\nIconGlyph 1.0 IconGlyph.qml\n");module.close();
        const auto icons=temporary.filePath("assets/icons");QVERIFY(QDir().mkpath(icons));
        QVERIFY(QFile::copy(QStringLiteral(LISTENFREE_TEST_SOURCE_DIR "/music_player_desktop/assets/album_Cover_2.png"),temporary.filePath("assets/album_Cover_2.png")));
        QVERIFY(QFile::copy(QStringLiteral(LISTENFREE_TEST_SOURCE_DIR "/music_player_desktop/assets/icons/music.svg"),icons+"/music.svg"));
        listenfree::media::ArtworkVideoFactory factory;
        QQmlEngine engine;engine.rootContext()->setContextProperty("backendArtworkVideoFactory",&factory);
        QQmlComponent component(&engine,QUrl::fromLocalFile(fixture+"/CoverArt.qml"));
        std::unique_ptr<QObject> root(component.create());QVERIFY2(root,qPrintable(component.errorString()));
        auto* item=qobject_cast<QQuickItem*>(root.get());QVERIFY(item);
        QQuickWindow window;window.resize(180,180);item->setParentItem(window.contentItem());item->setSize({180,180});
        item->setProperty("source",QUrl());window.show();QVERIFY(QTest::qWaitForWindowExposed(&window));
        item->setProperty("motionSource",QUrl::fromLocalFile(qEnvironmentVariable("LISTENFREE_TEST_COVER_VIDEO",QStringLiteral(LISTENFREE_TEST_SOURCE_DIR "/tests/fixtures/artwork-red-blue.mp4"))));
        QTRY_VERIFY_WITH_TIMEOUT(item->property("dynamicTexture").value<QQuickItem*>(),8000);
        auto* native=item->findChild<listenfree::media::ArtworkVideo*>();QVERIFY(native);QVERIFY(native->bounded());
        QVERIFY(item->findChildren<QMediaPlayer*>().isEmpty());
        auto* poster=item->property("dynamicFirstFrame").value<QQuickItem*>();QVERIFY(poster);
        const auto grab=[&] {
            const auto result=poster->grabToImage(QSize(64,64));
            QSignalSpy ready(result.get(),&QQuickItemGrabResult::ready);
            if(ready.isEmpty())ready.wait(2000);
            return result->image();
        };
        QTest::qWait(100);const auto first=grab();QVERIFY(!first.isNull());
        QVERIFY(first.pixelColor(32,32).red()>200);
        QSignalSpy loop(native,&listenfree::media::ArtworkVideo::looped);
        QTRY_VERIFY_WITH_TIMEOUT(loop.size()>0,5000);
        QCOMPARE(grab(),first);
        item->setProperty("motionPlaying",false);QTest::qWait(150);QCOMPARE(grab(),first);
        item->setProperty("motionSource",QUrl());
        QTRY_VERIFY(item->findChildren<listenfree::media::ArtworkVideo*>().isEmpty());
    }
};
QTEST_MAIN(ResourceOwnershipTests)
#include "resource_ownership_tests.moc"
