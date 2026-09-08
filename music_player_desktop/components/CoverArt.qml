import QtQuick
import QtQuick.Window
import QtQuick.Effects
import QtMultimedia

Item {
    id: root

    property url source: Qt.resolvedUrl("../assets/album_Cover_2.png")
    property url fallbackSource: ""
    property bool usingFallback: false
    property int recoveryAttempt: 0
    readonly property url effectiveSource: AppTheme.artworkUrl(root.usingFallback ? root.fallbackSource : root.source,root.sourcePixelSize)
    onEffectiveSourceChanged: { recoveryTimer.stop(); recoveryAttempt = 0; Qt.callLater(recoverIfMissing) }
    readonly property bool missingArtwork: sourceImage.status !== Image.Ready || (sourceImage.implicitWidth <= 1 && sourceImage.implicitHeight <= 1)
    onSourceChanged: usingFallback = false
    onFallbackSourceChanged: { usingFallback = false; tryFallback() }
    function tryFallback() {
        if (!usingFallback && String(fallbackSource).length && String(fallbackSource) !== String(source)
                && (sourceImage.status === Image.Error || (sourceImage.status === Image.Ready && sourceImage.implicitWidth <= 1 && sourceImage.implicitHeight <= 1)))
            usingFallback = true
    }
    function needsRecovery() {
        // Read Image directly: its status/size signals can run before the
        // derived missingArtwork binding has observed the successful decode.
        return root.visible && root.recoveryAttempt < 2
            && String(root.effectiveSource).indexOf("image://covers/") === 0
            && (sourceImage.status === Image.Error || (sourceImage.status === Image.Ready
                && sourceImage.implicitWidth <= 1 && sourceImage.implicitHeight <= 1))
    }
    function recoverIfMissing() {
        if (needsRecovery()) recoveryTimer.restart()
        else recoveryTimer.stop()
    }
    onVisibleChanged: recoverIfMissing()
    Component.onCompleted: recoverIfMissing()
    Timer {
        id: recoveryTimer
        interval: root.recoveryAttempt === 0 ? 350 : 1100
        // A pending retry must never discard an image that has since loaded.
        onTriggered: if (root.needsRecovery()) ++root.recoveryAttempt
    }
    readonly property Item dynamicTexture: motionLoader.item && motionLoader.item.ready ? motionLoader.item.output : null
    // Shape fills consume a cropped texture in logical item coordinates; the
    // layer normalizes source image dimensions and @2x density before sampling.
    readonly property Item artworkTexture: sourceImage
    property bool textureOnly: false
    readonly property Item dynamicFirstFrame: motionLoader.item && motionLoader.item.ready && motionLoader.item.frameCaptured ? motionLoader.item.firstFrame : null
    property url motionSource: ""
    property bool motionPlaying: true
    property string artworkTier: width <= 64 ? "Thumbnail" : width <= 128 ? "Small" : width <= 320 ? "Medium" : "Large"
    property real cornerRadius: 12
    // Compatibility for existing callers; covers now use bare rounded artwork.
    property bool showShadow: false
    // Hint the decoder so each usage tier can bound image memory explicitly.
    property int sourcePixelSize: AppTheme.artworkPixels(artworkTier, Screen.devicePixelRatio)

    // Keep a full-size backing even for successfully loaded transparent images.
    // It shares the cover bounds and has no inset shadow or outline.
    Rectangle {
        objectName: "coverPlaceholder"
        visible: !root.textureOnly
        anchors.fill: parent
        radius: root.cornerRadius
        color: "#8597a5"
        IconGlyph { anchors.centerIn: parent; width: Math.min(30,parent.width * .5); height: width; kind: "music"; glyphColor: "#e0ffffff"; visible: root.missingArtwork }
    }

    Image {
        id: sourceImage
        objectName: "coverSourceImage"
        anchors.fill: parent
        // A successful 1px missing-cover response is cached by Qt too. Retry
        // with a distinct key; keep retries uncached so a temporary failure
        // cannot poison subsequent delegates. Real covers keep normal caching.
        source: root.recoveryAttempt === 0 ? root.effectiveSource
            : String(root.effectiveSource) + (String(root.effectiveSource).indexOf("?") >= 0 ? "&" : "?") + "lf_retry=" + root.recoveryAttempt
        cache: root.recoveryAttempt === 0
        onStatusChanged: { root.tryFallback(); root.recoverIfMissing() }
        onImplicitWidthChanged: { root.tryFallback(); root.recoverIfMissing() }
        onImplicitHeightChanged: root.recoverIfMissing()
        sourceSize.width: root.sourcePixelSize
        sourceSize.height: root.sourcePixelSize
        fillMode: Image.PreserveAspectCrop
        smooth: true
        mipmap: true
        visible: false
        layer.enabled: root.textureOnly
        layer.smooth: true
    }

    Rectangle {
        id: maskItem
        anchors.fill: parent
        radius: root.cornerRadius
        visible: false
        layer.enabled: !root.textureOnly
    }

    Loader {
        id: motionLoader
        anchors.fill: parent
        active: root.visible && root.motionSource.toString().length > 0
        sourceComponent: Item {
            id: motion
            property alias output: video
            property alias firstFrame: firstFrameSample
            property bool frameCaptured: false
            property bool ready: movie.hasVideo && video.videoSink.videoSize.width > 0 && movie.mediaStatus !== MediaPlayer.InvalidMedia
            MediaPlayer {
                id: movie
                objectName: "dynamicArtworkMediaPlayer"
                source: root.motionSource
                autoPlay: root.motionPlaying
                loops: MediaPlayer.Infinite
                videoOutput: video
                onSourceChanged: motion.frameCaptured = false
            }
            Connections { target: root; function onMotionPlayingChanged() { if (root.motionPlaying) movie.play(); else movie.pause() } }
            VideoOutput { id: video; anchors.fill: parent; fillMode: VideoOutput.PreserveAspectCrop; visible: false }
            // Capture once per media source, independently of background mode or
            // window size. Cover playback and loop boundaries never refresh it.
            ShaderEffectSource {
                id: firstFrameSample
                objectName: "dynamicArtworkFirstFrame"
                // Keep a tiny covered item in the scene so capture also happens
                // while Solid/Flow is selected and no background consumes it.
                width: 1; height: 1
                textureSize: Qt.size(512, 512)
                sourceItem: video
                live: false
            }
            Connections {
                target: video.videoSink
                function onVideoFrameChanged() {
                    if (!motion.frameCaptured && video.videoSink.videoSize.width > 0) {
                        firstFrameSample.scheduleUpdate()
                        motion.frameCaptured = true
                    }
                }
            }
        }
    }

    MultiEffect {
        anchors.fill: parent
        visible: !root.textureOnly && (!root.missingArtwork || !!root.dynamicTexture)
        source: motionLoader.item && motionLoader.item.ready ? motionLoader.item.output : sourceImage
        maskEnabled: true
        maskSource: maskItem
        maskThresholdMin: 0.5
        maskSpreadAtMin: 1.0
    }

}
