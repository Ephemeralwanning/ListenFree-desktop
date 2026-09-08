import QtQuick
import QtTest
import "pages"

Item {
    width: 1066; height: 709
    property string route: "library/songs"
    property int saves: 0
    Rectangle {
        width: 233; height: parent.height; color: "#52606a"
        TapHandler { onTapped: route = "my-lists" }
    }
    MusicEditorDialog {
        id: editor
        anchors.fill: parent; z: 250
        track: ({title:"测试歌曲", artist:"测试艺术家", album:"测试专辑", artwork:""})
        reduceMotion: true
        onSaveRequested: { ++saves; open = false }
        onCloseRequested: open = false
    }
    TestCase {
        name: "EditorModalNavigation"
        when: windowShown
        function init() { route = "library/songs"; saves = 0; editor.open = true; wait(50) }
        function test_tabClicksThenSave() {
            for (let tab of [1,2,0]) {
                mouseClick(findChild(editor,"musicEditorTab"+tab))
                compare(editor.selectedTab,tab)
                compare(route,"library/songs","Editor tab must not activate the sidebar beneath it")
            }
            mouseClick(findChild(editor,"musicEditorSaveButton"))
            compare(saves,1)
            compare(editor.open,false)
            compare(route,"library/songs","Saving must leave the original page selected")
        }
        function test_tabThenCancel() {
            mouseClick(findChild(editor,"musicEditorTab1"))
            mouseClick(findChild(editor,"musicEditorCloseButton"))
            compare(editor.open,false)
            compare(saves,0)
            compare(route,"library/songs")
        }
    }
}
