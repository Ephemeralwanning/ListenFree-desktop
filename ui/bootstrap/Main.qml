import QtQuick
import QtQuick.Controls

ApplicationWindow {
    visible: true
    width: 900
    height: 600
    title: qsTr("ListenFree Bootstrap")

    Column {
        anchors.centerIn: parent
        spacing: 12
        Label {
            text: appController.ready ? qsTr("Backend ready") : qsTr("Starting backend")
            font.pixelSize: 24
        }
        Label {
            text: qsTr("Tracks: %1 | Queue: %2 | State: %3")
                  .arg(appController.tracksModel.rowCount())
                  .arg(appController.queueModel.rowCount())
                  .arg(appController.playbackState)
        }
    }
}
