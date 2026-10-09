#include <QtZpl/qtzpl.hpp>
#include <QtCore/QFile>
#include <QtGui/QTransform>
#include <QtTest/QTest>

using namespace Qt::StringLiterals;

class Font0RasterTest final : public QObject {
  Q_OBJECT
private slots:
  void hyphenMatricesMatchLabelary_data() {
    QTest::addColumn<QString>("name");
    QTest::newRow("small-asymmetric-and-rotated") << u"small-matrix"_s;
    QTest::newRow("large-and-fractional-pen-positions") << u"large-and-phases"_s;
    QTest::newRow("quantization-boundary") << u"boundary-probe"_s;
    QTest::newRow("fresh-holdout-after-model-freeze") << u"fresh-holdout"_s;
  }

  void hyphenMatricesMatchLabelary() {
    QFETCH(QString,name);
    const auto base=u":/font0-dashes/"_s+name;
    QFile file(base+u".zpl");QVERIFY(file.open(QIODevice::ReadOnly));
    const auto golden=QImage(base+u"-labelary-bitonal.png").convertToFormat(QImage::Format_RGB32);
    QVERIFY(!golden.isNull());
    const auto document=QtZpl::parse(QString::fromUtf8(file.readAll()));QVERIFY(document);
    // Independent original bitonal pages contain only hyphens and spaces.
    // Compare every pixel, including all margins; no similarity tolerance.
    for(const bool collectGeometry:{false,true}) {
      const auto result=QtZpl::render(*document,QtZpl::RenderOptions{
        .width=golden.width(),.height=golden.height(),.collectFieldGeometry=collectGeometry});
      QVERIFY(result);QVERIFY(result->diagnostics.isEmpty());QCOMPARE(result->labels.size(),1);
      QCOMPARE(result->labels.front().convertToFormat(QImage::Format_RGB32),golden);
    }
  }

  void minimumDimensionsMatchLabelarySemantics() {
    const QImage golden(u":/font0-semantics/minimum-size-labelary-bitonal.png"_s);
    QVERIFY(!golden.isNull());
    QFile file(u":/font0-semantics/minimum-size.zpl"_s); QVERIFY(file.open(QIODevice::ReadOnly));
    const auto rendered=QtZpl::render(QString::fromUtf8(file.readAll()));
    QVERIFY(rendered); QVERIFY(rendered->diagnostics.isEmpty());
    QCOMPARE(rendered->labels.front().size(),golden.size());
    // A separate Labelary fixture proves each axis clamps to 10 for FO/FT
    // and all four orientations. Compare like-for-like cropped placements,
    // independently of the remaining Font 0 contour mismatch with Labelary.
    for (int row=0;row<6;++row) for (int column=0;column<4;++column) {
      const QRect left(20+column*304,40+row*200,130,140);
      const QRect right=left.translated(140,0);
      QCOMPARE(golden.copy(left),golden.copy(right));
      QCOMPARE(rendered->labels.front().copy(left),rendered->labels.front().copy(right));
    }
    for (const auto origin : {u"FO"_s,u"FT"_s}) for (const auto orientation : QStringView(u"NRIB")) {
      const auto label=u"^XA^PW300^LL300^%1^A0%2,%3,%4^FDHIa^FS^XZ"_s;
      const auto minimum=QtZpl::render(label.arg(origin+u"100,100").arg(orientation).arg(10).arg(10));
      QVERIFY(minimum);
      for (int size=1;size<10;++size) {
        const auto small=QtZpl::render(label.arg(origin+u"100,100").arg(orientation).arg(size).arg(size));
        QVERIFY(small); QCOMPARE(small->labels,minimum->labels);
      }
    }
  }

  void directAndObservedRasterAgree_data() {
    QTest::addColumn<QString>("origin");
    QTest::addColumn<QChar>("orientation");
    QTest::addColumn<QColor>("ink");
    const QStringList origins{u"FO30,40"_s,u"FT30,100"_s,u"FO-12,-4"_s,
                              u"FT580,380"_s,u"FO2147483647,0"_s};
    const QList<QColor> colors{Qt::black,QColor(23,111,197),QColor(23,111,197,127),
                               QColor::fromRgba64(60000,12000,40000,65408),
                               QColor::fromRgba64(12345,34567,56789,65535)};
    for (const auto& origin : origins) for (const auto orientation : QStringView(u"NRIB"))
      for (qsizetype c = 0; c < colors.size(); ++c)
        QTest::newRow(qPrintable(origin + orientation + QString::number(c))) << origin << orientation << colors[c];
  }

  void directAndObservedRasterAgree() {
    QFETCH(QString,origin); QFETCH(QChar,orientation); QFETCH(QColor,ink);
    // Geometry collection deliberately retains the isolated field raster. Its
    // image is an independent composition path for the direct-paint fast path.
    for (const int height : {12,18,26,48,54}) for (const auto direction : QStringView(u"HVR")) {
      const auto zpl = u"^XA^CI28^PW640^LL384^FO10,10^GB620,360,3^FS^%1^A0%2,%3,%4"
                        u"^FP%5,2^FDNORTHLINE-Балтийск 0123 Жé^FS^XZ"_s
                          .arg(origin).arg(orientation).arg(height).arg(height * 3 / 4).arg(direction);
      QtZpl::RenderOptions options{.foreground=ink,.background=QColor(237,224,211,190)};
      const auto direct=QtZpl::render(zpl,{},options);
      options.collectFieldGeometry=true;
      const auto observed=QtZpl::render(zpl,{},options);
      QVERIFY(direct); QVERIFY(observed);
      QVERIFY(direct->diagnostics.isEmpty()); QVERIFY(observed->diagnostics.isEmpty());
      QCOMPARE(direct->labels,observed->labels);
    }
  }

  void largeCanvasKeepsCroppedRasterFallback() {
    for (const int x : {32550,32700,32768,39960}) {
      const auto source=u"^XA^PW40000^LL40^FO%1,5^A0N,12,12^FDTest123^FS^XZ"_s.arg(x);
      const auto direct=QtZpl::render(source);
      const auto observed=QtZpl::render(source,{},QtZpl::RenderOptions{.collectFieldGeometry=true});
      QVERIFY(direct); QVERIFY(observed);
      QVERIFY(direct->diagnostics.isEmpty()); QVERIFY(observed->diagnostics.isEmpty());
      QCOMPARE(direct->labels,observed->labels);
    }
  }

  void northlinePreservesPixelsAndDotGeometry() {
    QFile file(u":/northline/northline.zpl"_s); QVERIFY(file.open(QIODevice::ReadOnly));
    const auto document=QtZpl::parse(QString::fromUtf8(file.readAll())); QVERIFY(document);
    const auto observed=QtZpl::render(*document,QtZpl::RenderOptions{.collectFieldGeometry=true});
    QVERIFY(observed); QVERIFY(observed->diagnostics.isEmpty());
    QCOMPARE(observed->labels.size(),1); QCOMPARE(observed->labels.front().size(),QSize(799,799));
    for (const int dpi : {203,300,600}) {
      const auto direct=QtZpl::render(*document,QtZpl::RenderOptions{.dpi=dpi});
      QVERIFY(direct); QVERIFY(direct->diagnostics.isEmpty());
      QCOMPARE(direct->labels,observed->labels);
    }
    const auto& image=observed->labels.front();
    for (int y=0;y<image.height();++y) {
      const auto* row=reinterpret_cast<const QRgb*>(image.constScanLine(y));
      for (int x=0;x<image.width();++x)
        QVERIFY(row[x]==qRgb(0,0,0)||row[x]==qRgb(255,255,255));
    }
  }

  void northlineGraphicsMatchLabelary() {
    QFile file(u":/northline/northline-graphics.zpl"_s); QVERIFY(file.open(QIODevice::ReadOnly));
    const auto result=QtZpl::render(QString::fromUtf8(file.readAll()));
    QVERIFY(result); QVERIFY(result->diagnostics.isEmpty()); QCOMPARE(result->labels.size(),1);
    const QImage golden(u":/northline/northline-graphics-labelary-bitonal.png"_s);
    QVERIFY(!golden.isNull());
    QCOMPARE(result->labels.front().convertToFormat(QImage::Format_RGB32),golden.convertToFormat(QImage::Format_RGB32));
  }

  void baselineRotationMatchesLabelaryEdgeAnchors_data() {
    QTest::addColumn<int>("height");
    QTest::addColumn<int>("width");
    QTest::newRow("small-square") << 17 << 17;
    QTest::newRow("tall-condensed") << 64 << 23;
  }

  void baselineRotationMatchesLabelaryEdgeAnchors() {
    QFETCH(int,height); QFETCH(int,width);
    const auto base=u":/font0-anchors/anchors-h%1-w%2"_s.arg(height).arg(width);
    const auto golden=QImage(base+u"-labelary-bitonal.png").convertToFormat(QImage::Format_RGB32);
    QVERIFY(!golden.isNull());
    QFile file(base+u".zpl"); QVERIFY(file.open(QIODevice::ReadOnly));
    const auto result=QtZpl::render(QString::fromUtf8(file.readAll()));
    QVERIFY(result); QVERIFY(result->diagnostics.isEmpty()); QCOMPARE(result->labels.size(),1);
    const auto actual=result->labels.front().convertToFormat(QImage::Format_RGB32);
    QCOMPARE(actual.size(),golden.size());
    constexpr int cell=144;
    const auto inkBounds=[](const QImage& image) {
      QRect bounds;
      for(int y=0;y<image.height();++y) for(int x=0;x<image.width();++x)
        if(qRed(image.pixel(x,y))<128) bounds|=QRect(x,y,1,1);
      return bounds;
    };
    // Each atlas contains independent H/I/E/T/g/Ж/я glyphs, first FO then FT.
    // The Labelary reference establishes edge-based rotation around the FT
    // origin. Compare each implementation against its own normal glyph so
    // remaining contour differences cannot hide a one-dot placement error.
    for(int glyph=0;glyph<7;++glyph) {
      const auto normalRect=QRect(0,(glyph+7)*cell,cell,cell);
      const auto goldenNormal=golden.copy(normalRect);
      const auto actualNormal=actual.copy(normalRect);
      for(int orientation=0;orientation<4;++orientation) {
        const auto targetRect=normalRect.translated(orientation*cell,0);
        QTransform turn;turn.rotate(orientation*90);
        const auto expectedGolden=goldenNormal.transformed(turn,Qt::FastTransformation);
        const auto expectedActual=actualNormal.transformed(turn,Qt::FastTransformation);
        QCOMPARE(inkBounds(golden.copy(targetRect)),inkBounds(expectedGolden));
        QCOMPARE(actual.copy(targetRect),expectedActual);
        // At 17 dots Labelary also preserves every rotated pixel. At 64 dots
        // a few scan-conversion edge pixels differ, but all ink bounds agree.
        if(height==17) QCOMPARE(golden.copy(targetRect),expectedGolden);
        const auto foRect=targetRect.translated(0,-7*cell);
        const int ascent=height*3/4;
        const bool compareY=orientation==0||orientation==2;
        const int expectedSeparation=orientation==0||orientation==3?ascent:height-ascent;
        for(const auto& image : {golden,actual}) {
          const auto fo=inkBounds(image.copy(foRect));
          const auto ft=inkBounds(image.copy(targetRect));
          QVERIFY(!fo.isEmpty()); QVERIFY(!ft.isEmpty());
          QCOMPARE(compareY?fo.y()-ft.y():fo.x()-ft.x(),expectedSeparation);
        }
      }
    }
  }

  void blockBaselineRotationMatchesLabelaryEdgeAnchors() {
    const auto base=u":/font0-anchors/block-anchors"_s;
    const auto golden=QImage(base+u"-labelary-bitonal.png").convertToFormat(QImage::Format_RGB32);
    QVERIFY(!golden.isNull());
    QFile file(base+u".zpl"); QVERIFY(file.open(QIODevice::ReadOnly));
    const auto result=QtZpl::render(QString::fromUtf8(file.readAll()));
    QVERIFY(result); QVERIFY(result->diagnostics.isEmpty()); QCOMPARE(result->labels.size(),1);
    const auto actual=result->labels.front().convertToFormat(QImage::Format_RGB32);
    QCOMPARE(actual.size(),golden.size());
    const auto inkBounds=[](const QImage& image) {
      QRect bounds;
      for(int y=0;y<image.height();++y) for(int x=0;x<image.width();++x)
        if(qRed(image.pixel(x,y))<128) bounds|=QRect(x,y,1,1);
      return bounds;
    };
    constexpr int cell=384;
    // The multiline cases reserve three baselines but paint only two lines;
    // their rotated anchor must include the unpainted logical line as well.
    for(int row=0;row<3;++row) {
      const QRect normalRect(0,row*cell,cell,cell);
      for(int orientation=1;orientation<4;++orientation) {
        QTransform turn;turn.rotate(orientation*90);
        const auto targetRect=normalRect.translated(orientation*cell,0);
        const auto expectedGolden=golden.copy(normalRect).transformed(turn,Qt::FastTransformation);
        const auto expectedActual=actual.copy(normalRect).transformed(turn,Qt::FastTransformation);
        QCOMPARE(inkBounds(golden.copy(targetRect)),inkBounds(expectedGolden));
        if(row<2) QCOMPARE(golden.copy(targetRect),expectedGolden);
        QCOMPARE(actual.copy(targetRect),expectedActual);
      }
    }
  }
};

QTEST_MAIN(Font0RasterTest)
#include "test_font0_raster.moc"
