#pragma once
#include <QObject>
#include <QVariantList>
#include <QTextLayout>
#include <QTextBoundaryFinder>
#include <QFontMetricsF>
#include <algorithm>
#include <cmath>

// Article layout port of Folia Fume. See docs/research/immersive-native.md.
// Only metadata crosses QML; Text nodes are instantiated for visible blocks.
class FumeLayout : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList lyrics READ lyrics WRITE setLyrics NOTIFY layoutChanged)
    Q_PROPERTY(QVariantList blocks READ blocks NOTIFY layoutChanged)
    Q_PROPERTY(QString seed READ seed WRITE setSeed NOTIFY layoutChanged)
    Q_PROPERTY(QString fontFamily READ fontFamily WRITE setFontFamily NOTIFY layoutChanged)
public:
    explicit FumeLayout(QObject* parent=nullptr):QObject(parent){}
    QVariantList lyrics() const{return lyrics_;}
    QVariantList blocks() const{return blocks_;}
    QString seed() const{return seed_;}
    QString fontFamily() const{return family_;}
    void setLyrics(const QVariantList& v){if(v==lyrics_)return;lyrics_=v;rebuild();}
    void setSeed(const QString& v){if(v==seed_)return;seed_=v;rebuild();}
    void setFontFamily(const QString& v){if(v==family_)return;family_=v;rebuild();}
    Q_INVOKABLE QVariantMap block(int index) const{return index>=0&&index<blocks_.size()?blocks_[index].toMap():QVariantMap{};}
    static uint hash(const QString& text){uint h=2166136261u;for(const QChar c:text){h^=c.unicode();h*=16777619u;}return h;}
    static qreal start(const QVariantMap& line){return line.value("timeMs",line.value("timestampMs",line.value("time",0))).toDouble();}
Q_SIGNALS:
    void layoutChanged();
private:
    QVariantList lyrics_,blocks_;
    QString seed_,family_="Microsoft YaHei UI";
    void rebuild(){
        blocks_.clear();if(lyrics_.isEmpty()){emit layoutChanged();return;}
        QList<int> order;for(int i=0;i<lyrics_.size();++i){order.append(i);blocks_.append(QVariantMap{{"index",i},{"start",start(lyrics_[i].toMap())},{"end",start(lyrics_[i].toMap())},{"width",0},{"height",0}});}
        std::stable_sort(order.begin(),order.end(),[&](int a,int b){return hash(seed_+QString::number(a)+lyrics_[a].toMap().value("text").toString())%10000 < hash(seed_+QString::number(b)+lyrics_[b].toMap().value("text").toString())%10000;});
        constexpr qreal colWidth=360,gap=28; qreal bottoms[4]={0,0,0,0};
        for(int ordinal=0;ordinal<order.size();++ordinal){
            const int index=order[ordinal];const auto line=lyrics_[index].toMap();
            const QString text=line.value("text",line.value("en")).toString();
            if(text.trimmed().isEmpty())continue;
            const auto words=line.value("words").toList();
            const bool hero=(text.size()<=28&&((ordinal+1)%6==0||line.value("isChorus").toBool())) || (order.size()<6 && ordinal==order.size()/2);
            const qreal width=hero?colWidth*2+gap:colWidth;
            QFont font(family_);font.setWeight(hero?QFont::ExtraBold:QFont::DemiBold);
            // Folia fits a complete line into a column, using large hero blocks.
            qreal size=hero?72:40;
            font.setPixelSize(qRound(size));
            size=std::clamp(size*width/std::max(width,QFontMetricsF(font).horizontalAdvance(text)),hero?32.:18.,hero?72.:40.);
            font.setPixelSize(qRound(size));
            QTextLayout layout(text,font);QTextOption option;option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);layout.setTextOption(option);
            layout.beginLayout();qreal height=0;QList<QTextLine> textLines;
            while(true){auto tl=layout.createLine();if(!tl.isValid())break;tl.setLineWidth(width);tl.setPosition({0,height});height+=tl.height()*1.06;textLines.append(tl);}layout.endLayout();
            int col=0;for(int c=1;c<(hero?3:4);++c){const qreal value=hero?std::max(bottoms[c],bottoms[c+1]):bottoms[c];const qreal best=hero?std::max(bottoms[col],bottoms[col+1]):bottoms[col];if(value<best)col=c;}
            const qreal y=hero?std::max(bottoms[col],bottoms[col+1]):bottoms[col];
            bottoms[col]=y+height+22;if(hero)bottoms[col+1]=bottoms[col];
            qreal end=index+1<lyrics_.size()?start(lyrics_[index+1].toMap()):start(line)+5000;
            for(const auto& v:words)end=std::max(end,v.toMap().value("endMs").toDouble());
            QVariantList glyphs;QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme,text);int from=0,to=0,wordIndex=0,wordBegin=0,wordEnd=words.isEmpty()?0:words.first().toMap().value("text").toString().size();
            while((to=finder.toNextBoundary())>=0){
                while(wordIndex+1<words.size()&&from>=wordEnd){wordBegin=wordEnd;wordIndex++;wordEnd+=words[wordIndex].toMap().value("text").toString().size();}
                qreal begin=start(line),finish=end;
                if(!words.isEmpty()){const auto word=words[wordIndex].toMap();const qreal ws=word.value("startMs").toDouble(),we=word.value("endMs").toDouble();begin=ws+(we-ws)*std::clamp(qreal(from-wordBegin)/std::max(1,wordEnd-wordBegin),0.,1.);finish=ws+(we-ws)*std::clamp(qreal(to-wordBegin)/std::max(1,wordEnd-wordBegin),0.,1.);}
                for(const auto& tl:textLines)if(from>=tl.textStart()&&from<tl.textStart()+tl.textLength()){
                    const qreal left=tl.cursorToX(from),right=tl.cursorToX(to);
                    glyphs.append(QVariantMap{{"text",text.mid(from,to-from)},{"x",std::min(left,right)},{"y",tl.y()},{"width",std::abs(right-left)},{"height",tl.height()},{"start",begin},{"end",finish},{"timed",!words.isEmpty()}});break;
                }from=to;
            }
            blocks_[index]=QVariantMap{{"index",index},{"text",text},{"x",col*(colWidth+gap)},{"y",y},{"width",width},{"height",height},{"fontSize",qRound(size)},{"hero",hero},{"start",start(line)},{"end",end},{"glyphs",glyphs}};
        }emit layoutChanged();
    }
};
