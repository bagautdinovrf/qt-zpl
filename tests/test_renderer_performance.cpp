#include <QtTest/QTest>
#include <QtZpl/qtzpl.hpp>
#include <QtGui/QPainter>
#include <future>
#include <limits>

using namespace Qt::StringLiterals;

class RendererPerformanceTest final : public QObject {
  Q_OBJECT

private slots:
  void graphicFieldPreservesPainterComposition_data() {
    QTest::addColumn<QPoint>("origin");
    QTest::addColumn<QColor>("foreground");
    QTest::addColumn<QColor>("background");
    QTest::addColumn<bool>("binary");
    const QColor highPrecisionAlpha=QColor::fromRgba64(60000,12000,40000,65408);
    // Its 8-bit alpha rounds to opaque, but the original 16-bit alpha is not.
    QCOMPARE(highPrecisionAlpha.alpha(),255);
    QVERIFY(highPrecisionAlpha.rgba64().alpha()<65535);
    const QList<QPoint> positions{
      QPoint(0,0),QPoint(-3,-1),QPoint(9,6),QPoint(-30,-10),
      QPoint(std::numeric_limits<int>::max(),std::numeric_limits<int>::max()),
      QPoint(std::numeric_limits<int>::min(),std::numeric_limits<int>::min())
    };
    const QList<QColor> foregrounds{QColor(21,96,170),QColor(21,96,170,100),
      QColor(21,96,170,0),highPrecisionAlpha,QColor::fromRgba64(12345,34567,56789,65535)};
    const QList<QColor> backgrounds{QColor(245,220,190),QColor(200,140,80,90)};
    for(qsizetype p=0;p<positions.size();++p)
      for(qsizetype f=0;f<foregrounds.size();++f)
        for(qsizetype b=0;b<backgrounds.size();++b)
          for(bool binary:{false,true}) {
            const auto name=u"position-%1-ink-%2-paper-%3-%4"_s
              .arg(p).arg(f).arg(b).arg(binary?u"binary"_s:u"ascii"_s);
            QTest::newRow(qPrintable(name))<<positions[p]<<foregrounds[f]<<backgrounds[b]<<binary;
          }
  }

  void graphicFieldPreservesPainterComposition() {
    QFETCH(QPoint,origin);QFETCH(QColor,foreground);QFETCH(QColor,background);QFETCH(bool,binary);
    // Include marker bytes in binary data and a partial final binary row.
    const QByteArray bytes=QByteArray::fromHex(binary?"81FF00245A5E7E":"81FF00245A5E");
    const QString data=QString::fromLatin1(binary?bytes:bytes.toHex());
    const auto zpl=u"^XA^PW17^LL8^FO%1,%2^GF%3,%4,%4,2,"_s
      .arg(origin.x()).arg(origin.y()).arg(binary?u"B"_s:u"A"_s).arg(bytes.size())
      +data+u"^FS^XZ"_s;
    const auto result=QtZpl::render(zpl,{},QtZpl::RenderOptions{
      .foreground=foreground,.background=background});
    QVERIFY(result.has_value());QCOMPARE(result->labels.size(),1);QVERIFY(result->diagnostics.isEmpty());

    QImage expected(17,8,QImage::Format_ARGB32_Premultiplied);expected.fill(background);
    QPainter painter(&expected);painter.setPen(foreground);
    // The reference enumerates every source bit and uses the previous painter
    // composition path. Wide coordinates also cover entirely off-label fields.
    for(qsizetype i=0;i<bytes.size();++i)for(int bit=0;bit<8;++bit) {
      if(!(static_cast<unsigned char>(bytes[i])&(0x80U>>bit)))continue;
      const qint64 x=static_cast<qint64>(origin.x())+(i%2)*8+bit;
      const qint64 y=static_cast<qint64>(origin.y())+i/2;
      if(x>=0&&x<expected.width()&&y>=0&&y<expected.height())
        painter.drawPoint(static_cast<int>(x),static_cast<int>(y));
    }
    painter.end();
    QCOMPARE(result->labels.front(),expected);
  }

  void font0ContextPreservesSeparateFieldsAndLabels() {
    const QStringList fields{
      u"^FO15,15^A0N,27,34^FDМолоко ±°Ёё^FS"_s,
      u"^FT30,150^A0R,40,24^FDABC0123^FS"_s,
      u"^FO280,260^A0B,20,35^FB180,2,3,C^FDLine one\\&Line two^FS"_s,
      u"^FO10,370^BY1,2,40^A0N,20,12^BCN,40,Y,N,N,A^FD12345678^FS"_s
    };
    const QString start=u"^XA^PW600^LL500"_s;
    const QString end=u"^XZ"_s;
    QImage expected(600,500,QImage::Format_ARGB32_Premultiplied);expected.fill(Qt::white);
    for(const auto& field:fields) {
      const auto separate=QtZpl::render(start+field+end);
      QVERIFY(separate.has_value());QVERIFY(separate->diagnostics.isEmpty());
      // The fields occupy disjoint rectangles; multiply keeps each opaque
      // field's ink while removing the white canvas around it.
      QPainter painter(&expected);painter.setCompositionMode(QPainter::CompositionMode_Multiply);
      painter.drawImage(0,0,separate->labels.front());
    }
    const QString label=start+fields.join(QString{})+end;
    const auto document=QtZpl::parse(label+label);QVERIFY(document.has_value());
    const auto combined=QtZpl::render(*document);QVERIFY(combined.has_value());
    QVERIFY(combined->diagnostics.isEmpty());QCOMPARE(combined->labels.size(),2);
    QCOMPARE(combined->labels[0],expected);QCOMPARE(combined->labels[1],expected);
    const auto run=[&document]{return QtZpl::render(*document);};
    auto first=std::async(std::launch::async,run);
    auto second=std::async(std::launch::async,run);
    const auto a=first.get(),b=second.get();
    QVERIFY(a.has_value());QVERIFY(b.has_value());
    QCOMPARE(a->labels,combined->labels);QCOMPARE(b->labels,combined->labels);
  }

  void font0GlyphCachePreservesSizesAndOrientations() {
    QString stream;
    QList<QImage> independentlyRendered;
    const QList<QSize> sizes{QSize(9,18),QSize(45,30),QSize(30,60),QSize(34,27)};
    for(const auto size:sizes)for(const auto orientation:QStringView{u"NRIB"})
      for(bool baseline:{false,true}) {
        const auto label=u"^XA^PW600^LL500^%1%2,180^A0%3,%4,%5^FDAbc-0123 ±Ёё^FS^XZ"_s
          .arg(baseline?u"FT"_s:u"FO"_s).arg(baseline?300:100)
          .arg(orientation).arg(size.height()).arg(size.width());
        const auto independent=QtZpl::render(label);
        QVERIFY(independent.has_value());QVERIFY(independent->diagnostics.isEmpty());
        independentlyRendered.append(independent->labels.front());
        stream+=label;
      }
    const auto combined=QtZpl::render(stream);QVERIFY(combined.has_value());
    QCOMPARE(combined->labels,independentlyRendered);
  }
};

QTEST_MAIN(RendererPerformanceTest)
#include "test_renderer_performance.moc"
