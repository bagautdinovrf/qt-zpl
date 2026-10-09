#include <QtZpl/qtzpl.hpp>
#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSet>
#include <QtGui/QRawFont>
#include <QtTest/QTest>
#include <algorithm>
#include <cstdlib>
#include "../src/font0_advance_data.hpp"

using namespace Qt::StringLiterals;

namespace {
QByteArray resource(const QString& name) {
  QFile file(u":/font0-metrics/"_s + name);
  return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
}
QJsonObject json(const QString& name) {
  return QJsonDocument::fromJson(resource(name)).object();
}
QRect inkBounds(const QImage& image, const QRect& requested) {
  const QRect area = requested.intersected(image.rect());
  int left = area.right()+1, right = area.left()-1;
  int top = area.bottom()+1, bottom = area.top()-1;
  for (int y=area.top(); y<=area.bottom(); ++y) {
    const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
    for (int x=area.left(); x<=area.right(); ++x) if (qGray(row[x])<128) {
      left=std::min(left,x); right=std::max(right,x);
      top=std::min(top,y); bottom=std::max(bottom,y);
    }
  }
  return right>=left ? QRect(QPoint(left,top),QPoint(right,bottom)) : QRect{};
}
QRect cellBounds(const QImage& image, const QJsonObject& cell) {
  return inkBounds(image,QRect(0,cell[u"top"_s].toInt(),image.width(),
    cell[u"bottom"_s].toInt()-cell[u"top"_s].toInt()));
}
bool solidInk(const QImage& image, const QRect& rect) {
  if (!image.rect().contains(rect)) return false;
  for (int y=rect.top();y<=rect.bottom();++y) {
    const auto* row=reinterpret_cast<const QRgb*>(image.constScanLine(y));
    for (int x=rect.left();x<=rect.right();++x) if (qGray(row[x])>=128) return false;
  }
  return true;
}
}

class Font0MetricsTest final : public QObject {
  Q_OBJECT
private slots:
  void embeddedGlyphIdsAndFallbacks() {
    const auto coverage=json(u"embedded-font-coverage.json"_s);
    const auto bytes=resource(u"font0.ttf"_s);
    QVERIFY(!bytes.isEmpty());
    QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex()),
             coverage[u"fontSha256"_s].toString());
    const QRawFont font(bytes,1000,QFont::PreferNoHinting);
    QVERIFY(font.isValid());
    const auto glyphIds=coverage[u"glyphIds"_s].toObject();
    QCOMPARE(glyphIds.size(),304);
    const auto measured=json(u"measurements.json"_s)[u"glyphs"_s].toObject();
    QSet<int> covered;
    for (auto it=glyphIds.begin(); it!=glyphIds.end(); ++it) {
      const char32_t scalar=it.key().mid(2).toUInt(nullptr,16);
      const auto glyphs=font.glyphIndexesForString(QString::fromUcs4(&scalar,1));
      QCOMPARE(glyphs.size(),1);
      QCOMPARE(glyphs.front(),quint32(it.value().toInt()));
      covered.insert(int(glyphs.front()));
      QCOMPARE(QtZpl::Font0AdvanceData::advances2048[glyphs.front()],
               measured[it.key()].toObject()[u"hmtxAdvance"_s].toInt());
    }
    const auto absent=coverage[u"absentCodepoints"_s].toArray();
    QCOMPARE(absent.size(),13);
    for (const auto cp:absent) {
      const char32_t scalar=cp.toString().mid(2).toUInt(nullptr,16);
      QCOMPARE(font.glyphIndexesForString(QString::fromUcs4(&scalar,1)).front(),quint32(0));
    }
    for (qsizetype glyph=0; glyph<qsizetype(QtZpl::Font0AdvanceData::advances2048.size()); ++glyph)
      if (!covered.contains(int(glyph))) QCOMPARE(QtZpl::Font0AdvanceData::advances2048[glyph],qint16(-1));
  }

  void repeatedGlyphAdvanceMatchesOriginalOracle_data() {
    QTest::addColumn<QJsonObject>("entry");
    for (const auto value:json(u"manifest.json"_s)[u"cases"_s].toArray()) {
      const auto entry=value.toObject();
      QTest::newRow(qPrintable(entry[u"name"_s].toString())) << entry;
    }
  }
  void repeatedGlyphAdvanceMatchesOriginalOracle() {
    QFETCH(QJsonObject,entry);
    const auto name=entry[u"name"_s].toString();
    const auto source=resource(name+u".zpl"_s);
    const auto png=resource(name+u"-labelary-bitonal.png"_s);
    QVERIFY(!source.isEmpty()); QVERIFY(!png.isEmpty());
    QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(source,QCryptographicHash::Sha256).toHex()),entry[u"zplSha256"_s].toString());
    QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(png,QCryptographicHash::Sha256).toHex()),
             entry[u"response"_s].toObject()[u"pngSha256"_s].toString());
    const QImage golden=QImage::fromData(png).convertToFormat(QImage::Format_RGB32);
    QVERIFY(!golden.isNull());
    const auto rendered=QtZpl::render(QString::fromUtf8(source));
    QVERIFY(rendered); QVERIFY(rendered->diagnostics.isEmpty());
    QCOMPARE(rendered->labels.size(),1);
    const QImage actual=rendered->labels.front().convertToFormat(QImage::Format_RGB32);
    QCOMPARE(actual.size(),golden.size());
    const auto cells=entry[u"cells"_s].toArray();
    QVERIFY(cells.size()>1);
    const QRect goldenMarker=cellBounds(golden,cells.first().toObject());
    const QRect actualMarker=cellBounds(actual,cells.first().toObject());
    QVERIFY(!goldenMarker.isEmpty()); QVERIFY(!actualMarker.isEmpty());
    const auto supported=json(u"embedded-font-coverage.json"_s)[u"glyphIds"_s].toObject();
    int checked=0;
    for (qsizetype index=1; index<cells.size(); ++index) {
      const auto cell=cells[index].toObject();
      const auto cp=cell[u"codepoint"_s].toString();
      if (!supported.contains(cp)) continue; // Explicitly recorded missing current-font glyphs.
      const QRect expected=cellBounds(golden,cell);
      const QRect observed=cellBounds(actual,cell);
      QVERIFY(!expected.isEmpty()); QVERIFY(!observed.isEmpty());
      const int expectedDistance=expected.right()-goldenMarker.right();
      if (entry[u"fontWidth"_s].toInt()==300 && (cp==u"U+2030"_s || cp==u"U+2116"_s)) {
        // Preserved Labelary defect: these wide glyphs disappear without a
        // diagnostic. Width100/200 independent rulers cover their advances.
        QCOMPARE(expectedDistance,0);
        continue;
      }
      // The bundled middle-dot/bullet contours are wider than the reference
      // advance and can extend beyond the following pipe at their midline.
      // Its top scanline is isolated from those contours. Use the same
      // scanline within the standalone pipe and the translated field row.
      const int probeY=cell[u"top"_s].toInt()+actualMarker.top()-cells.first().toObject()[u"top"_s].toInt();
      const QRect probe=inkBounds(actual,QRect(0,probeY,actual.width(),1));
      const QRect referenceProbe=inkBounds(actual,QRect(0,actualMarker.top(),actual.width(),1));
      QVERIFY(!probe.isEmpty()); QVERIFY(!referenceProbe.isEmpty());
      const int actualDistance=probe.right()-referenceProbe.right();
      QVERIFY2(std::abs(actualDistance-expectedDistance)<=1,
        qPrintable(u"%1: original oracle advance %2, rendered %3 dots"_s.arg(cp).arg(expectedDistance).arg(actualDistance)));
      ++checked;
    }
    QVERIFY(checked>0);
  }

  void foFtAnchorsMatchOriginalOracle_data() {
    QTest::addColumn<QString>("name"); QTest::addColumn<int>("height");
    for (const auto size:{QSize(22,14),QSize(9,16),QSize(38,25),QSize(33,26),QSize(34,27),QSize(40,40),QSize(251,70)}) {
      const auto name=u"font0-h%1-w%2"_s.arg(size.height()).arg(size.width());
      QTest::newRow(qPrintable(name)) << name << size.height();
    }
  }
  void foFtAnchorsMatchOriginalOracle() {
    QFETCH(QString,name); QFETCH(int,height);
    const QImage golden=QImage::fromData(resource(name+u"-labelary-bitonal.png"_s)).convertToFormat(QImage::Format_RGB32);
    QVERIFY(!golden.isNull());
    const auto rendered=QtZpl::render(QString::fromUtf8(resource(name+u".zpl"_s)));
    QVERIFY(rendered); QVERIFY(rendered->diagnostics.isEmpty());
    const QImage actual=rendered->labels.front().convertToFormat(QImage::Format_RGB32);
    QCOMPARE(actual.size(),golden.size());
    for (const auto& image:{golden,actual}) for (int row=0; row<4; ++row) {
      const int foY=20+row*130,ftY=680+row*130;
      const QRect fo=inkBounds(image,QRect(0,foY-20,image.width(),130));
      const QRect ft=inkBounds(image,QRect(0,ftY-100,image.width(),130));
      QVERIFY(!fo.isEmpty()); QVERIFY(!ft.isEmpty());
      QCOMPARE(fo.left(),ft.left());
      QCOMPARE((fo.top()-foY)-(ft.top()-ftY),(height*3)/4);
      QCOMPARE(image.copy(fo),image.copy(ft));
    }
  }

  void cursorContinuationAndJustificationMatchOriginalOracle_data() {
    QTest::addColumn<QString>("fixture");
    QTest::newRow("orientations-and-anchors") << u"layout-holdout"_s;
    QTest::newRow("independent-parameters") << u"layout-parameter-holdout"_s;
    QTest::newRow("fo-rotations") << u"layout-fo-rotations"_s;
    QTest::newRow("fo-independent-parameters-and-empty") << u"layout-fo-parameter-holdout"_s;
  }
  void cursorContinuationAndJustificationMatchOriginalOracle() {
    QFETCH(QString,fixture);
    const auto manifest=json(fixture+u".json"_s);
    const auto source=resource(fixture+u".zpl"_s);
    const auto png=resource(fixture+u"-labelary-bitonal.png"_s);
    const auto provenance=manifest[u"response"_s].toObject();
    QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(source,QCryptographicHash::Sha256).toHex()),provenance[u"zplSha256"_s].toString());
    QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(png,QCryptographicHash::Sha256).toHex()),provenance[u"pngSha256"_s].toString());
    const QImage golden=QImage::fromData(png).convertToFormat(QImage::Format_RGB32);
    QVERIFY(!golden.isNull());
    const auto rendered=QtZpl::render(QString::fromUtf8(source),{},QtZpl::RenderOptions{.collectFieldGeometry=true});
    QVERIFY(rendered); QVERIFY(rendered->diagnostics.isEmpty());
    const QImage actual=rendered->labels.front().convertToFormat(QImage::Format_RGB32);
    QCOMPARE(actual.size(),golden.size());
    QList<QRect> actualMarkers;
    for (const auto& field:rendered->fields)
      if (field.field.kind==QtZpl::FieldKind::Box) actualMarkers.append(field.paintBounds);
    QCOMPARE(actualMarkers.size(),manifest[u"cases"_s].toArray().size()*2);
    QStringList discrepancies;
    int checked=0;
    for (const auto value:manifest[u"cases"_s].toArray()) {
      const auto entry=value.toObject();
      const auto cell=entry[u"cell"_s].toArray();
      const int x=cell[0].toInt(), y=cell[1].toInt();
      // After repeating the source field, partial FT overrides one coordinate
      // and carries the other into an isolated 3x11 solid graphic marker.
      // Empty top/left strips separate cursor placement from glyph contours.
      QList<QRect> expectedMarkers;
      if (entry.contains(u"markerRects"_s)) {
        // Original connected solid markers are recorded for the dense final
        // holdout, where a valid continuation crosses a nominal cell boundary.
        // Their coordinates were measured from the saved PNG, not the table.
        for (const auto value:entry[u"markerRects"_s].toArray()) {
          const auto rect=value.toArray();
          expectedMarkers.append(QRect(rect[0].toInt(),rect[1].toInt(),rect[2].toInt(),rect[3].toInt()));
        }
      } else {
        expectedMarkers.append(inkBounds(golden,QRect(x+40,y,cell[2].toInt()-40,30)));
        expectedMarkers.append(inkBounds(golden,QRect(x,y+30,30,cell[3].toInt()-30)));
      }
      QCOMPARE(expectedMarkers.size(),2);
      for (const auto& expected:expectedMarkers) {
        const QRect observed=actualMarkers[checked];
        QCOMPARE(expected.size(),QSize(3,11));
        QVERIFY(solidInk(golden,expected));
        QVERIFY(solidInk(actual,observed));
        if (observed.size()!=expected.size() || std::abs(observed.x()-expected.x())>1 || std::abs(observed.y()-expected.y())>1)
          discrepancies.append(u"%1 %2 FP%3 justification%4 '%5': marker expected(%6,%7), actual(%8,%9)"_s
            .arg(entry[u"anchor"_s].toString(),entry[u"orientation"_s].toString(),entry[u"direction"_s].toString())
            .arg(entry[u"justification"_s].toInt()).arg(entry[u"text"_s].toString())
            .arg(expected.x()-x).arg(expected.y()-y).arg(observed.x()-x).arg(observed.y()-y));
        ++checked;
      }
    }
    QCOMPARE(checked,manifest[u"cases"_s].toArray().size()*2);
    QVERIFY2(discrepancies.isEmpty(),qPrintable(discrepancies.join(u'\n')));
  }
};

QTEST_MAIN(Font0MetricsTest)
#include "test_font0_metrics.moc"
