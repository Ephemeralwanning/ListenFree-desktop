import QtQuick
import QtTest
import "Components"

Item {
    width: 240; height: 200
    Rectangle { anchors.fill: parent; color: "#667366" }
    CoverArt { id: cover; x: 20; y: 20; width: 132; height: 132; source: "" }
    TestCase {
        name: "CoverPlaceholder"
        when: windowShown
        function test_missing_data() {
            return [
                { tag: "empty", source: "" },
                { tag: "failed", source: Qt.resolvedUrl("nonexistent.png") },
                { tag: "transparent-sentinel", source: Qt.resolvedUrl("transparent.png") }
            ]
        }
        function test_missing(data) {
            cover.source = data.source
            wait(200)
            if (data.tag !== "transparent-sentinel")
                verify(cover.missingArtwork, "A missing image must show the placeholder")
            const frame = grabImage(cover)
            // The old five-pixel inset shadow left a different band at the top.
            compare(frame.pixel(66, 2), frame.pixel(66, 9), "Placeholder must fill the outline evenly")
            compare(frame.pixel(66, 9), frame.pixel(66, 120))
        }
        function test_loaded() {
            cover.source = Qt.resolvedUrl("opaque.png")
            tryCompare(cover, "missingArtwork", false)
            // Offscreen rendering validates the backing; actual GPU cover
            // pixels are checked by --cover-grid-only in the application.
        }
    }
}
