pragma ComponentBehavior: Bound
import QtQuick

Rectangle {
    id: panel
    MouseArea {
        anchors.fill: parent
        acceptedButtons: Qt.AllButtons
        hoverEnabled: true
        onWheel: wheel => wheel.accepted = true
    }
    objectName: "downloadPanel"
    property var controller
    property string deletingId: ""
    signal closeRequested()
    color: AppTheme.darkMode ? "#27313a" : "#f5f7f9"
    border.color: AppTheme.border
    function status(row) {
        const labels = { queued:qsTr("等待下载"), resolving:qsTr("获取音源"), downloading:qsTr("下载中"), paused:qsTr("已暂停"), finalizing:qsTr("写入标签与歌词"), completed:qsTr("已完成"), cancelled:qsTr("已取消"), error:qsTr("下载失败") }
        const size = row.received > 0 ? " · " + (row.received / 1048576).toFixed(1) + " MB" : ""
        return (labels[row.state] || row.state) + size + (row.error ? " · " + row.error : "")
    }
    MouseArea { anchors.fill: parent; onClicked: panel.forceActiveFocus() }
    Text { x: 22; y: 20; text: qsTr("下载管理"); color: AppTheme.textPrimary; font.family: AppTheme.fontFamily; font.pixelSize: 23; font.weight: Font.DemiBold }
    RoundIconButton { anchors.right: parent.right; anchors.rightMargin: 14; y: 16; diameter: 34; kind: "close"; onClicked: panel.closeRequested() }
    Row {
        x: 20; y: 65; spacing: 12
        UiButton { label: qsTr("全部暂停"); onClicked: panel.controller.pauseAll() }
        UiButton { label: qsTr("清空记录"); onClicked: panel.controller.clearRecords() }
    }
    Text { x: 22; y: 110; text: qsTr("按添加时间排序 · 清空记录保留文件"); color: AppTheme.textSecondary; font.pixelSize: 11 }
    ListView {
        id: list
        x: 12; y: 138; width: parent.width - 24; height: parent.height - y - 12
        clip: true; spacing: 5
        model: panel.controller ? panel.controller.tasks : []
        Text { anchors.centerIn: parent; visible: list.count === 0; text: qsTr("暂无下载任务"); color: AppTheme.textSecondary }
        delegate: Rectangle {
            id: row
            required property var modelData
            width: list.width; height: 79; radius: 11
            color: AppTheme.control
            CoverArt { x: 10; y: 15; width: 43; height: 43; cornerRadius: 7; source: row.modelData.artwork || ""; sourcePixelSize: 96; showShadow: false }
            Column {
                x: 65; y: 12; width: parent.width - 151; spacing: 5
                Text { width: parent.width; text: row.modelData.title || qsTr("歌曲"); elide: Text.ElideRight; color: AppTheme.textPrimary; font.pixelSize: 14; font.weight: Font.DemiBold }
                Text { width: parent.width; text: panel.status(row.modelData); elide: Text.ElideRight; color: AppTheme.textSecondary; font.pixelSize: 10 }
                Rectangle { width: parent.width; height: 3; radius: 2; color: AppTheme.border
                    Rectangle { height: 3; radius: 2; color: AppTheme.accent; width: parent.width * (row.modelData.total > 0 ? Math.min(1, row.modelData.received / row.modelData.total) : 0) }
                }
            }
            Row {
                anchors.right: parent.right; anchors.rightMargin: 7; anchors.verticalCenter: parent.verticalCenter; spacing: 1
                RoundIconButton {
                    diameter: 32
                    enabled: row.modelData.state !== "finalizing"
                    kind: row.modelData.state === "completed" ? "folder" : ["downloading","resolving","queued"].indexOf(row.modelData.state) >= 0 ? "pause" : "play"
                    onClicked: {
                        const s = row.modelData.state, id = row.modelData.id
                        if (s === "completed") panel.controller.locate(id)
                        else if (["downloading","resolving","queued"].indexOf(s) >= 0) panel.controller.pause(id)
                        else panel.controller.resume(id)
                    }
                }
                RoundIconButton { diameter: 32; enabled: row.modelData.state !== "finalizing"; kind: "close"; onClicked: {
                    if (row.modelData.state === "completed") panel.deletingId = row.modelData.id
                    else panel.controller.cancel(row.modelData.id)
                } }
            }
        }
    }
    AlertDialog {
        anchors.fill: parent
        open: panel.deletingId.length > 0
        title: qsTr("删除下载文件")
        message: qsTr("将此音频移入回收站，并移除下载记录。")
        primaryLabel: qsTr("删除"); secondaryLabel: qsTr("取消")
        onAccepted: { panel.controller.deleteFile(panel.deletingId); panel.deletingId = "" }
        onRejected: panel.deletingId = ""
    }
}
