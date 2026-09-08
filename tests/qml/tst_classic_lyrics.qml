import QtQuick
import QtTest
import ListenFree.Native 1.0
import "Components"

Item {
    width: 900; height: 700
    Rectangle { anchors.fill: parent; color: "#283343" }
    MouseArea { anchors.fill: parent; onWheel: wheel => wheel.accepted = true }
    LyricsPanel { id: panel; x: 100; y: 30; width: 650; height: 670; artworkBackground: true }
    LyricTextMetrics { id: metrics }
    Text { id: fontProbe; visible: false; font.family: "Segoe UI"; font.pixelSize: 30; font.weight: Font.DemiBold }
    TestCase {
        name: "ClassicLyricsMotion"
        when: windowShown
        function fixture() {
            const lines = []
            for (let i=0;i<24;++i) lines.push({text:"Keep this moment 停留",timeMs:i*4000,translation:"让这一刻停留",
                words:[{text:"Keep this moment ",startMs:i*4000,endMs:i*4000+400},
                       {text:"停留",startMs:i*4000+400,endMs:i*4000+3400}]})
            return lines
        }
        function init() {
            panel.animationActive = false
            panel.reducedMotion = false
            panel.playing = false
            panel.lyrics = fixture()
            panel.currentLineIndex = 3
            panel.positionMs = 12500
            panel.notifySeek()
            wait(100)
        }
        function effectCounts(item) {
            let glyphs=String(item.objectName).indexOf("lyricGrapheme")===0?1:0
            let textures=item.sourceItem!==undefined && item.sourceItem && item.hideSource===true
                && item.live===false && String(item.sourceItem.objectName).indexOf("lyricWord")===0?1:0
            const children=item.children || []
            for(let i=0;i<children.length;++i) {
                const child=effectCounts(children[i]);glyphs+=child.glyphs;textures+=child.textures
            }
            return {glyphs:glyphs,textures:textures}
        }
        function test_effect_budget() {
            panel.animationActive=true
            wait(500)
            const single=effectCounts(panel)
            compare(single.glyphs,2)
            compare(single.textures,1,"Two glyphs share one shaped token texture")
            panel.currentLineIndex=4
            wait(80)
            const overlap=effectCounts(panel)
            compare(overlap.glyphs,4)
            compare(overlap.textures,2,"Only current/outgoing words retain emphasis textures")
            panel.animationActive=false
            wait(50)
            compare(effectCounts(panel).textures,0)
            compare(effectCounts(panel).glyphs,0)
        }
        function test_graphemes_and_merged_syllables() {
            const font = fontProbe.font
            const tokens = metrics.prepare([
                {text:"sta",startMs:0,endMs:700},
                {text:"ying ",startMs:700,endMs:1800},
                {text:"A\u0301👩‍👩‍👧‍👦",startMs:1800,endMs:4800}],font)
            compare(tokens[0].emphasized,true)
            compare(tokens[0].groupCount,7)
            compare(tokens[1].charOffset,3)
            compare(tokens[0].emphasisDuration,tokens[1].emphasisDuration)
            compare(tokens[2].glyphs.length,2,"Combining accents and ZWJ emoji stay intact")
            compare(tokens[2].emphasized,false,"Long UTF-16 token must not qualify merely because it is long")
            const cjk = metrics.prepare([{text:"𠀀𠀁𠀂𠀃",startMs:0,endMs:1600}],font)
            compare(cjk[0].emphasized,true,"Supplementary CJK follows CJK duration rule")
            compare(cjk[0].glyphs.length,4)
        }
        function test_long_word_stagger_and_static_width() {
            panel.animationActive = true
            wait(550)
            const a=findChild(panel,"lyricGrapheme3/1/0"), b=findChild(panel,"lyricGrapheme3/1/1")
            verify(a);verify(b)
            const row=findChild(panel,"lyricRow3"), base=findChild(panel,"lyricWord3/1")
            const width=base.implicitWidth, height=row.height
            panel.positionMs=13600
            wait(100)
            verify(a.envelope>b.envelope,"Characters must not pulse in unison")
            verify(a.scale>1.01,"Long tone expands")
            verify(a.glowAlpha>0,"Long tone gains a local glow")
            compare(base.implicitWidth,width)
            compare(row.height,height,"Transforms cannot change row layout")
            grabImage(panel).save("build/classic-lyrics-check/long-word.png")
            const held=a.scale
            wait(200)
            compare(a.scale,held,"Paused media time freezes the emphasis")
            panel.positionMs=19000
            wait(100)
            compare(a.scale,1,"Seek evaluates the finished envelope immediately")
            panel.reducedMotion=true
            wait(50)
            verify(!findChild(panel,"lyricGrapheme3/1/0"),"Reduced motion releases glyph effects")
        }
        function test_wheel_holds_visual_focus() {
            const list=findChild(panel,"lyricList")
            const initial=list.contentY
            mouseWheel(panel,300,300,0,-240)
            wait(200)
            verify(Math.abs(list.contentY-initial)>10)
            const position=list.contentY
            compare(panel.focusLineIndex,3)
            panel.currentLineIndex=5
            wait(300)
            compare(panel.focusLineIndex,3,"Browsing freezes focus as well as scroll")
            compare(list.contentY,position)
            compare(panel.touchBrowsing,false,"Discrete wheel does not clear inactive blur")
            wait(5200)
            compare(panel.focusLineIndex,5)
            verify(Math.abs(list.contentY-position)>10)
        }
        function test_wheel_spring_and_continuous_drag() {
            panel.animationActive=true
            wait(500)
            const list=findChild(panel,"lyricList"), row=findChild(panel,"lyricRow3")
            mouseWheel(panel,300,300,0,-120)
            verify(Math.abs(row.renderedY-row.targetY)>10,"Discrete wheel retains motion rather than snapping")
            wait(120)
            const midway=row.renderedY
            wait(120)
            verify(Math.abs(row.renderedY-midway)>1,"Wheel spring continues towards its target")
            panel.suspendFollow(true)
            list.contentY+=80
            wait(20)
            verify(Math.abs(row.renderedY-row.targetY)<.1,"Continuous drag follows the pointer directly")
        }
        function test_gap_timing_and_relayout() {
            const gaps=[100,400,800]
            for (let j=0;j<gaps.length;++j) {
                const lines=fixture()
                for (let i=0;i<lines.length;++i) lines[i].timeMs=i*gaps[j]
                panel.lyrics=lines
                panel.currentLineIndex=3
                panel.positionMs+=100
                wait(100)
                const k=panel.lineSpringStiffness
                if(j===0) compare(k,220)
                if(j===1) verify(k>170 && k<220)
                if(j===2) compare(k,170)
            }
            panel.fontSize=40
            panel.showTranslation=false
            wait(150)
            const row=findChild(panel,"lyricRow3"), list=findChild(panel,"lyricList")
            verify(Math.abs(row.y+row.height/2-list.contentY-list.height/2)<1,"Font/translation change keeps the active line aligned")
            panel.fontSize=28;panel.showTranslation=true
        }
        function test_touch_focus_and_dense_lines() {
            panel.suspendFollow(true)
            panel.currentLineIndex=6
            wait(100)
            compare(panel.focusLineIndex,3)
            compare(findChild(panel,"lyricRow4").blurAmount,0)
            panel.resumeFollow()
            wait(100)
            compare(panel.focusLineIndex,6)
            panel.positionMs+=200
            panel.currentLineIndex=8
            wait(50)
            compare(panel.seeking,false,"Skipping dense lines is not a user seek")
        }
        function test_focus_mask_handoff_and_hide() {
            panel.animationActive=true
            wait(500)
            const old=findChild(panel,"lyricRow3")
            panel.currentLineIndex=4
            wait(80)
            verify(old.focusAmount>0 && old.focusAmount<1,"Outgoing word mask fades instead of vanishing")
            wait(500)
            compare(old.focusAmount,0)
            verify(!findChild(panel,"lyricGrapheme3/1/0"),"Finished rows release emphasis quads")
            panel.animationActive=false
            wait(50)
            verify(!findChild(panel,"lyricGrapheme4/1/0"),"Hidden pages release emphasis quads")
        }
    }
}
