#include <QtZpl/qtzpl.hpp>
#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>
#include <QtCore/QSet>
#include <QtGui/QImage>
#include <QtTest/QTest>
#include <algorithm>

using namespace Qt::StringLiterals;

namespace {
constexpr auto prefix=":/qtzpl/tests/golden/font-glyphs/";
QJsonArray rectJson(const QRect& rect) {return {rect.x(),rect.y(),rect.width(),rect.height()};}
QRect inkBounds(const QImage& image,const QRect& area) {
    QRect bounds;
    for(int y=area.top();y<=area.bottom();++y){
        const auto* row=reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for(int x=area.left();x<=area.right();++x)if(qGray(row[x])<128)bounds|=QRect(x,y,1,1);
    }
    return bounds;
}
qsizetype differences(const QImage& actual,const QImage& expected,const QRect& area) {
    qsizetype count=0;
    for(int y=area.top();y<=area.bottom();++y){
        const auto* a=reinterpret_cast<const QRgb*>(actual.constScanLine(y));
        const auto* e=reinterpret_cast<const QRgb*>(expected.constScanLine(y));
        for(int x=area.left();x<=area.right();++x)count+=(qGray(a[x])<128)!=(qGray(e[x])<128);
    }
    return count;
}
}

class FontGlyphsTest final : public QObject {
    Q_OBJECT
    QJsonObject manifest_;
    QJsonArray results_;
    QJsonArray failures_;
    QSet<QString> unsupported_;
private slots:
    void initTestCase() {
        QFile file(QString::fromLatin1(prefix)+u"manifest.json"_s);QVERIFY(file.open(QIODevice::ReadOnly));
        QJsonParseError error;
        const auto document=QJsonDocument::fromJson(file.readAll(),&error);
        QCOMPARE(error.error,QJsonParseError::NoError);QVERIFY(document.isObject());
        manifest_=document.object();
        QVERIFY(!manifest_[u"cases"_s].toArray().isEmpty());
        for(const auto value:manifest_[u"unsupportedCharacters"_s].toArray())unsupported_.insert(value.toString());
        for(const auto cp:{u"U+037E",u"U+2044",u"U+FB01",u"U+FB02"})QVERIFY(unsupported_.contains(QString(cp)));
    }
    void glyphAtlas_data() {
        QTest::addColumn<int>("caseIndex");
        const auto cases=manifest_[u"cases"_s].toArray();
        for(qsizetype i=0;i<cases.size();++i)QTest::newRow(qPrintable(cases[i].toObject()[u"name"_s].toString()))<<int(i);
    }
    void glyphAtlas() {
        QFETCH(int,caseIndex);
        const auto profile=manifest_[u"cases"_s].toArray()[caseIndex].toObject();
        const auto name=profile[u"name"_s].toString();
        const QString base=QString::fromLatin1(prefix)+name;
        QFile file(base+u".zpl"_s);QVERIFY(file.open(QIODevice::ReadOnly));
        const auto expected=QImage(base+u"-labelary-bitonal.png"_s).convertToFormat(QImage::Format_RGB32);
        QVERIFY(!expected.isNull());
        QCOMPARE(expected.size(),QSize(manifest_[u"width"_s].toInt(),manifest_[u"height"_s].toInt()));
        const auto rendered=QtZpl::render(QString::fromUtf8(file.readAll()),{},
            QtZpl::RenderOptions{.width=expected.width(),.height=expected.height()});
        QVERIFY2(rendered.has_value(),"The glyph atlas could not be rendered");
        QCOMPARE(rendered->labels.size(),1);
        const auto actual=rendered->labels.front().convertToFormat(QImage::Format_RGB32);
        QCOMPARE(actual.size(),expected.size());
        QSet<QString> expectedMissing;
        QJsonArray glyphResults;
        int diagnosticFailures=0;
        const auto recordFailure=[&](QJsonObject failure){
            failure[u"case"_s]=name;failure[u"fontHeight"_s]=profile[u"fontHeight"_s];failure[u"fontWidth"_s]=profile[u"fontWidth"_s];
            failures_.append(failure);
        };
        for(const auto value:profile[u"cells"_s].toArray()){
            const auto cell=value.toObject();const auto codepoint=cell[u"codepoint"_s].toString();
            // Include negative side bearings/accents while retaining the
            // manifest's full descent margin. Whole-atlas parity also checks
            // every pixel outside these diagnostic cell rectangles.
            const QRect area=QRect(cell[u"x"_s].toInt(),cell[u"y"_s].toInt(),cell[u"width"_s].toInt(),cell[u"height"_s].toInt())
                                 .adjusted(-10,-10,0,0).intersected(actual.rect());
            const auto count=differences(actual,expected,area);
            QJsonObject glyph{{u"codepoint"_s,codepoint},{u"differentPixels"_s,qint64(count)},
                              {u"actualInk"_s,rectJson(inkBounds(actual,area))},{u"expectedInk"_s,rectJson(inkBounds(expected,area))}};
            if(count){auto failure=glyph;failure[u"reason"_s]=u"pixel-mismatch"_s;recordFailure(failure);}
            if(unsupported_.contains(codepoint)){
                expectedMissing.insert(codepoint);
                const bool diagnostic=std::ranges::any_of(rendered->diagnostics,[&](const auto& d){
                    return d.code==u"font-glyph-missing"&&d.message.contains(codepoint,Qt::CaseInsensitive)&&d.offset>=0&&!d.command.isEmpty();
                });
                glyph[u"missingGlyphDiagnostic"_s]=diagnostic;
                if(!diagnostic){++diagnosticFailures;recordFailure({{u"codepoint"_s,codepoint},{u"reason"_s,u"missing-glyph-diagnostic-absent"_s}});}
                if(!inkBounds(expected,area).isEmpty()){
                    ++diagnosticFailures;recordFailure({{u"codepoint"_s,codepoint},{u"reason"_s,u"oracle-unsupported-cell-not-empty"_s}});
                }
            }
            glyphResults.append(glyph);
        }
        QJsonArray diagnosticResults;
        for(const auto& diagnostic:rendered->diagnostics){
            const QJsonObject item{{u"code"_s,diagnostic.code},{u"message"_s,diagnostic.message},
                                   {u"offset"_s,qint64(diagnostic.offset)},{u"command"_s,diagnostic.command}};
            diagnosticResults.append(item);
            const bool expectedDiagnostic=diagnostic.code==u"font-glyph-missing"&&std::ranges::any_of(expectedMissing,[&](const QString& cp){return diagnostic.message.contains(cp,Qt::CaseInsensitive);});
            if(!expectedDiagnostic){++diagnosticFailures;auto failure=item;failure[u"reason"_s]=u"unexpected-diagnostic"_s;recordFailure(failure);}
        }
        const auto total=differences(actual,expected,actual.rect());
        results_.append(QJsonObject{{u"case"_s,name},{u"fontHeight"_s,profile[u"fontHeight"_s]},{u"fontWidth"_s,profile[u"fontWidth"_s]},
                                   {u"differentPixels"_s,qint64(total)},{u"glyphs"_s,glyphResults},{u"diagnostics"_s,diagnosticResults}});
        if(total)actual.save(QCoreApplication::applicationDirPath()+u"/glyph-"_s+name+u"-actual.png"_s);
        QCOMPARE(diagnosticFailures,0);
        QCOMPARE(total,0);
    }
    void oversizedGraphics_data() {
        QTest::addColumn<QString>("command");
        for(const auto value:{u"^GC2147483647,1",u"^GE20,2147483647,1",u"^GD2147483647,20,1",u"^GD20,20,2147483647"})
            QTest::newRow(qPrintable(QString(value)))<<QString(value);
    }
    void oversizedGraphics() {
        QFETCH(QString,command);
        const auto result=QtZpl::render(u"^XA^PW40^LL40^FO0,0"_s+command+u"^FS^XZ"_s);
        QVERIFY(result);QCOMPARE(result->labels.size(),1);
        QVERIFY(std::ranges::any_of(result->diagnostics,[](const auto& diagnostic){return diagnostic.code==u"graphic-dimensions"&&diagnostic.offset>=0;}));
        const auto image=result->labels.front().convertToFormat(QImage::Format_RGB32);
        QVERIFY(inkBounds(image,image.rect()).isEmpty());
    }
    void clippedGraphicAtExtremeAnchor() {
        const auto result=QtZpl::render(u"^XA^PW40^LL40^FT-2147483648,-2147483648,1^GC30,2^FS^XZ");
        QVERIFY(result);QVERIFY(result->diagnostics.isEmpty());
        const auto image=result->labels.front().convertToFormat(QImage::Format_RGB32);
        QVERIFY(inkBounds(image,image.rect()).isEmpty());
    }
    void cleanupTestCase() {
        QSaveFile report(QCoreApplication::applicationDirPath()+u"/font-glyph-results.json"_s);
        QVERIFY(report.open(QIODevice::WriteOnly));
        const QJsonObject result{{u"oracle"_s,u"Labelary Bitonal"_s},{u"cases"_s,results_},{u"failures"_s,failures_},
                                 {u"unsupportedCharacters"_s,manifest_[u"unsupportedCharacters"_s]}};
        QVERIFY(report.write(QJsonDocument(result).toJson(QJsonDocument::Indented))>0);QVERIFY(report.commit());
    }
};

QTEST_MAIN(FontGlyphsTest)
#include "test_font_glyphs.moc"
