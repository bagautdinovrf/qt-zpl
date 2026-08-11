#include <QtTest/QTest>
#include <QtZpl/qtzpl.hpp>
#include "../src/barcode_encoders.hpp"
#include "../src/pdf417_encoder.hpp"
#include "../src/maxicode_encoder.hpp"
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtGui/QPen>
#include <algorithm>
#include <cmath>
#include <limits>

using namespace Qt::StringLiterals;

class QtZplTest final : public QObject {
  Q_OBJECT
  static QString goldenZpl(QStringView name) {
    QFile file(u":/qtzpl/tests/golden/"_s+name+u".zpl"_s);
    if(!file.open(QIODevice::ReadOnly))return {};
    return QString::fromUtf8(file.readAll());
  }
  static QImage goldenImage(QStringView name) {
    return QImage(u":/qtzpl/tests/golden/"_s+name+u"-labelary-bitonal.png"_s);
  }
  static QByteArray corpusFile(QStringView name) {
    QFile file(u":/qtzpl/tests/corpus/go-zpl-demo/"_s+name);
    if(!file.open(QIODevice::ReadOnly))return {};
    return file.readAll();
  }
  static QJsonArray corpusExamples() {
    return QJsonDocument::fromJson(corpusFile(u"manifest.json")).object().value(u"examples"_s).toArray();
  }
  static QImage corpusGolden(QStringView fixture,int page) {
    const auto stem=QFileInfo(fixture.toString()).completeBaseName();
    return QImage(u":/qtzpl/tests/golden/go-zpl-demo/%1-page-%2-labelary-bitonal.png"_s.arg(stem).arg(page+1));
  }
  static double inkJaccard(const QImage& actual,const QImage& golden,QRect region={}) {
    if(actual.size()!=golden.size())return 0.0;
    if(region.isEmpty())region=actual.rect();
    qsizetype intersection=0;
    qsizetype inkUnion=0;
    for(int y=region.top();y<=region.bottom();++y)for(int x=region.left();x<=region.right();++x){
      const bool actualInk=actual.pixelColor(x,y).value()<128;
      const bool goldenInk=golden.pixelColor(x,y).value()<128;
      intersection+=actualInk&&goldenInk;
      inkUnion+=actualInk||goldenInk;
    }
    return inkUnion>0?static_cast<double>(intersection)/static_cast<double>(inkUnion):1.0;
  }
  static QRect inkBounds(const QImage& image) {
    QRect bounds;
    for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)
      if(image.pixelColor(x,y).value()<128)bounds|=QRect(x,y,1,1);
    return bounds;
  }
private slots:
  void demoCorpusCompatibility_data() {
    QTest::addColumn<QString>("fixture");
    QTest::addColumn<int>("dpi");
    QTest::addColumn<int>("width");
    QTest::addColumn<int>("height");
    QTest::addColumn<bool>("ignoreLabelHome");
    QTest::addColumn<int>("pages");
    const auto examples=corpusExamples();
    QCOMPARE(examples.size(),14);
    for(const auto& value:examples){
      const auto entry=value.toObject();
      const auto key=entry.value(u"key"_s).toString();
      QTest::newRow(qPrintable(key))
        <<entry.value(u"fixture"_s).toString()
        <<entry.value(u"dpi"_s).toInt()
        <<entry.value(u"widthDots"_s).toInt()
        <<entry.value(u"heightDots"_s).toInt()
        <<entry.value(u"ignoreLabelHome"_s).toBool()
        <<entry.value(u"pages"_s).toInt();
    }
  }
  void demoCorpusCompatibility() {
    QFETCH(QString,fixture);QFETCH(int,dpi);QFETCH(int,width);QFETCH(int,height);
    QFETCH(bool,ignoreLabelHome);QFETCH(int,pages);
    const auto bytes=corpusFile(fixture);QVERIFY2(!bytes.isEmpty(),qPrintable(fixture));
    const auto zpl=QString::fromLatin1(bytes);
    const auto document=QtZpl::parse(zpl);QVERIFY(document.has_value());
    QCOMPARE(document->labels().size(),pages);
    const auto result=QtZpl::render(*document,QtZpl::RenderOptions{
      .dpi=dpi,.width=width,.height=height,.ignoreLabelHome=ignoreLabelHome});
    QVERIFY(result.has_value());QCOMPARE(result->labels.size(),pages);
    for(const auto& image:result->labels)QCOMPARE(image.size(),QSize(width,height));
    QStringList forbiddenDiagnostics;
    for(const auto& diagnostic:result->diagnostics){
      const bool forbidden=diagnostic.severity==QtZpl::Severity::Error
        ||diagnostic.code==u"unsupported-command"_s
        ||diagnostic.code==u"barcode-render-pending"_s;
      if(forbidden)forbiddenDiagnostics.append(diagnostic.code+u" "_s+diagnostic.command);
    }
    QVERIFY2(forbiddenDiagnostics.isEmpty(),qPrintable(fixture+u": "_s+forbiddenDiagnostics.join(u", "_s)));
  }
  void demoCorpusGoldenParity() {
    QStringList mismatches;
    for(const auto& value:corpusExamples()){
      const auto entry=value.toObject();const auto key=entry.value(u"key"_s).toString();const auto fixture=entry.value(u"fixture"_s).toString();
      const auto bytes=corpusFile(fixture);QVERIFY2(!bytes.isEmpty(),qPrintable(fixture));
      const auto result=QtZpl::render(QString::fromLatin1(bytes),{},QtZpl::RenderOptions{
        .dpi=entry.value(u"dpi"_s).toInt(),.width=entry.value(u"widthDots"_s).toInt(),.height=entry.value(u"heightDots"_s).toInt(),
        .ignoreLabelHome=entry.value(u"ignoreLabelHome"_s).toBool()});
      QVERIFY(result.has_value());
      for(int page=0;page<result->labels.size();++page){
        const auto golden=corpusGolden(fixture,page);QVERIFY2(!golden.isNull(),qPrintable(fixture));const auto& actual=result->labels[page];
        QCOMPARE(actual.size(),golden.size());qsizetype different=0;
        for(int y=0;y<actual.height();++y)for(int x=0;x<actual.width();++x)
          different+=(actual.pixelColor(x,y).value()<128)!=(golden.pixelColor(x,y).value()<128);
        const double similarity=inkJaccard(actual,golden);
        const auto actualBounds=inkBounds(actual),goldenBounds=inkBounds(golden);
        if(different!=0||actualBounds!=goldenBounds)mismatches.append(
          u"%1[%2]: pixels=%3 jaccard=%4 actual=(%5,%6 %7x%8) golden=(%9,%10 %11x%12)"_s
            .arg(key).arg(page+1).arg(different).arg(similarity,0,'f',6)
            .arg(actualBounds.x()).arg(actualBounds.y()).arg(actualBounds.width()).arg(actualBounds.height())
            .arg(goldenBounds.x()).arg(goldenBounds.y()).arg(goldenBounds.width()).arg(goldenBounds.height()));
      }
    }
    QVERIFY2(mismatches.isEmpty(),qPrintable(mismatches.join(u"\n"_s)));
  }
  void parsesMultipleLabels() {
    auto result=QtZpl::parse(u"^XA^PW100^LL80^FO2,3^FDOne^FS^XZ^XA^FDTwo^FS^XZ");
    QVERIFY(result.has_value()); QCOMPARE(result->labels().size(),2);
    QCOMPARE(result->labels()[0].width(),100); QCOMPARE(result->labels()[0].height(),80);
  }
  void preservesUnknownCommands() {
    auto result=QtZpl::parse(u"^XA^ZZabc^XZ"); QVERIFY(result.has_value());
    QCOMPARE(result->diagnostics().size(),1); QCOMPARE(result->diagnostics()[0].code,u"unsupported-command"_s);
  }
  void rendersGeometry() {
    auto result=QtZpl::render(u"^XA^PW40^LL30^FO5,5^GB20,10,2,B^FS^XZ");
    QVERIFY(result.has_value()); QCOMPARE(result->labels.size(),1); QCOMPARE(result->labels[0].size(),QSize(40,30));
    QCOMPARE(result->labels[0].pixelColor(5,5),QColor(Qt::black));
  }
  void graphicPrimitiveBoundsMatchLabelary() {
    struct Case{QString zpl;QRect bounds;};
    const QList<Case> cases{
      {u"^XA^PW140^LL140^FO10,10^GB100,100,2^FS^XZ"_s,QRect(10,10,100,100)},
      {u"^XA^PW140^LL140^FO10,10^GC80,5^FS^XZ"_s,QRect(10,11,80,79)},
      {u"^XA^PW180^LL100^FO10,10^GE150,60,3^FS^XZ"_s,QRect(10,11,150,59)},
    };
    for(const auto& test:cases){const auto result=QtZpl::render(test.zpl);QVERIFY(result.has_value());QCOMPARE(inkBounds(result->labels.front()),test.bounds);}
  }
  void graphicDiagonalsMatchShapesGolden() {
    const auto bytes=corpusFile(u"shapes.zpl");const auto golden=corpusGolden(u"shapes.zpl",0);QVERIFY(!bytes.isEmpty());QVERIFY(!golden.isNull());
    const auto result=QtZpl::render(QString::fromLatin1(bytes),{},QtZpl::RenderOptions{.width=812,.height=609,.ignoreLabelHome=true});QVERIFY(result.has_value());
    const QList<QRect> regions{QRect(40,440,125,125),QRect(160,440,125,125),QRect(280,440,180,110)};
    for(const auto& region:regions){qsizetype different=0;for(int y=region.top();y<=region.bottom();++y)for(int x=region.left();x<=region.right();++x)
      different+=(result->labels[0].pixelColor(x,y).value()<128)!=(golden.pixelColor(x,y).value()<128);QCOMPARE(different,0);}
  }
  void graphicShapesMatchDemoGolden() {
    const auto bytes=corpusFile(u"shapes.zpl");const auto golden=corpusGolden(u"shapes.zpl",0);QVERIFY(!bytes.isEmpty());QVERIFY(!golden.isNull());
    const auto result=QtZpl::render(QString::fromLatin1(bytes),{},QtZpl::RenderOptions{.width=812,.height=609,.ignoreLabelHome=true});QVERIFY(result.has_value());
    const QList<QRect> regions{QRect(40,70,480,115),QRect(40,220,400,105),QRect(40,330,400,110),QRect(490,240,270,170)};
    for(const auto& region:regions){qsizetype different=0;for(int y=region.top();y<=region.bottom();++y)for(int x=region.left();x<=region.right();++x)
      different+=(result->labels[0].pixelColor(x,y).value()<128)!=(golden.pixelColor(x,y).value()<128);QCOMPARE(different,0);}
  }
  void rejectsInvalidDpi() {
    auto document=QtZpl::parse(u"^XA^XZ"); QVERIFY(document.has_value());
    auto result=QtZpl::render(*document,{.dpi=200}); QVERIFY(!result.has_value());
  }
  void rendersEan13() {
    auto result=QtZpl::render(u"^XA^PW300^LL180^BY2,3,100^FO10,10^BEN,100,N,N^FD5901234123457^FS^XZ");
    QVERIFY(result.has_value());
    QVERIFY(std::none_of(result->diagnostics.cbegin(),result->diagnostics.cend(),[](const auto& d){return d.code==u"barcode-render-pending"_s;}));
    QCOMPARE(result->labels[0].pixelColor(10,10),QColor(Qt::black));
    QCOMPARE(result->labels[0].pixelColor(12,10),QColor(Qt::white));
  }
  void code128CodewordsMatchKnownVector() {
    const auto words=QtZpl::BarcodeEncoders::Detail::code128Codewords(u"ABC123",u'N');
    QVERIFY(words.has_value());
    QCOMPARE(*words,QVector<int>({104,33,34,35,17,18,19,67}));
    const auto modules=QtZpl::BarcodeEncoders::code128(u"ABC123"_s,u'N');
    QVERIFY(modules.has_value());
    QByteArray widths;
    for(qsizetype i=0;i<modules->size();){
      const bool value=(*modules)[i];qsizetype end=i+1;
      while(end<modules->size()&&(*modules)[end]==value)++end;
      widths.append(static_cast<char>('0'+end-i));i=end;
    }
    QCOMPARE(widths,QByteArrayLiteral("2112141113231311231313211232212232112211321411222331112"));
  }
  void codabarModulesMatchIsoVector() {
    const auto modules=QtZpl::BarcodeEncoders::codabar(u"A12B",3);
    QVERIFY(modules.has_value());
    QByteArray runs;
    for(qsizetype i=0;i<modules->size();){const bool bit=(*modules)[i];qsizetype end=i+1;while(end<modules->size()&&(*modules)[end]==bit)++end;runs.append(char('0'+end-i));i=end;}
    QCOMPARE(runs,QByteArrayLiteral("1133131111113311111311311313113"));
  }
  void pdf417HighLevelCodewordsMatchIsoVectors() {
    using QtZpl::BarcodeEncoders::Pdf417::highLevelCodewords;
    QCOMPARE(*highLevelCodewords("01234"),QVector<int>({902,112,434}));
    QCOMPARE(*highLevelCodewords("Super !"),QVector<int>({567,615,137,809,329}));
    QCOMPARE(*highLevelCodewords("ABC123"),QVector<int>({1,88,32,119}));
  }
  void pdf417ReedSolomonMatchesIsoVector() {
    const QVector<int> input{16,902,1,278,827,900,295,902,2,326,823,544,900,149,900,900};
    QCOMPARE(QtZpl::BarcodeEncoders::Pdf417::errorCorrection(input,2),QVector<int>({628,715,393,299,863,601,169,708}));
  }
  void pdf417ModulesHaveExactFedexGeometry() {
    const auto symbol=QtZpl::BarcodeEncoders::Pdf417::encode("PDF417 Demo",2,0,0);
    QVERIFY(symbol.has_value());QCOMPARE(symbol->matrix.width,(symbol->columns+4)*17+1);QCOMPARE(symbol->matrix.height,symbol->rows);
    QVERIFY(symbol->matrix.at(0,0));QVERIFY(symbol->matrix.at(1,0));QVERIFY(!symbol->matrix.at(8,0));
  }
  void maxicodeMode2PrimaryAndGridMatchIsoPlacement() {
    QByteArray data="000000000000000[)>";data.append(char(0x1e));data+="01";data.append(char(0x1d));data+="96TRACK";data.append(char(0x1d));data+="UPSN";data.append(char(0x1e));data+="07DATA";data.append(char(0x1e));data.append(char(0x04));
    const auto rebuilt=QtZpl::BarcodeEncoders::MaxiCode::reconstructMode2(data);QVERIFY(rebuilt.has_value());
    const QByteArray primaryFields=QByteArray("96")+QByteArray("000000000")+char(0x1d)+"000"+char(0x1d)+"000"+char(0x1d);
    QVERIFY(rebuilt->contains(primaryFields));
    const auto symbol=QtZpl::BarcodeEncoders::MaxiCode::encodeMode2(data);QVERIFY(symbol.has_value());
    QCOMPARE(symbol->grid.width,30);QCOMPARE(symbol->grid.height,33);
    QCOMPARE(symbol->codewords.mid(0,10),QVector<int>({2,0,0,0,0,16,2,0,0,0}));
    QVERIFY(symbol->grid.at(28,0));QVERIFY(symbol->grid.at(10,9));QVERIFY(symbol->grid.at(17,23));
  }
  void qrCodeMatchesHelloDemoGolden() {
    const auto bytes=corpusFile(u"hello.zpl");const auto golden=corpusGolden(u"hello.zpl",0);QVERIFY(!bytes.isEmpty());QVERIFY(!golden.isNull());
    const auto result=QtZpl::render(QString::fromLatin1(bytes),{},QtZpl::RenderOptions{.width=812,.height=609,.ignoreLabelHome=true});QVERIFY(result.has_value());
    const QRect region(50,100,145,165);qsizetype different=0;
    for(int y=region.top();y<=region.bottom();++y)for(int x=region.left();x<=region.right();++x)
      different+=(result->labels[0].pixelColor(x,y).value()<128)!=(golden.pixelColor(x,y).value()<128);
    QCOMPARE(different,0);
  }
  void qrVcardMatchesDemoGolden() {
    const auto bytes=corpusFile(u"qrcode.zpl");const auto golden=corpusGolden(u"qrcode.zpl",0);
    QVERIFY(!bytes.isEmpty());QVERIFY(!golden.isNull());
    const qsizetype qrCommand=bytes.indexOf("^BQN,2,4");QVERIFY(qrCommand>=0);
    const qsizetype field=bytes.indexOf("^FD",qrCommand);const qsizetype end=bytes.indexOf("^FS",field);
    QVERIFY(field>=0&&end>field);
    QByteArray payload=bytes.sliced(field+3,end-field-3);
    QVERIFY(payload.startsWith("MM,A"));payload.remove(0,4);payload=payload.toUpper();
    payload.removeIf([](char value){
      return !QByteArrayView{"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:"}.contains(value);
    });
    int bestMask=-1;qsizetype bestModuleDifference=std::numeric_limits<qsizetype>::max();
    for(int mask=0;mask<8;++mask){
      const auto matrix=QtZpl::BarcodeEncoders::qrCode(payload,u'M',mask);QVERIFY(matrix.has_value());
      if(matrix->width!=45)continue;
      qsizetype difference=0;
      for(int y=0;y<45;++y)for(int x=0;x<45;++x)
        difference+=matrix->at(x,y)!=(golden.pixelColor(50+x*4+2,210+y*4+2).value()<128);
      if(difference<bestModuleDifference){bestModuleDifference=difference;bestMask=mask;}
    }
    QCOMPARE(bestMask,6);
    QCOMPARE(bestModuleDifference,0);
    const auto result=QtZpl::render(QString::fromLatin1(bytes),{},QtZpl::RenderOptions{
      .width=812,.height=609,.ignoreLabelHome=true});
    QVERIFY(result.has_value());
    const QRect region(50,210,180,180);qsizetype different=0;
    for(int y=region.top();y<=region.bottom();++y)for(int x=region.left();x<=region.right();++x)
      different+=(result->labels[0].pixelColor(x,y).value()<128)!=(golden.pixelColor(x,y).value()<128);
    QCOMPARE(different,0);
  }
  void remainingDemoQrCodesMatchGoldens_data() {
    QTest::addColumn<QString>("fixture");
    QTest::addColumn<QRect>("region");
    QTest::newRow("barcodes")<<u"barcodes.zpl"_s<<QRect(500,175,84,84);
    QTest::newRow("product")<<u"product.zpl"_s<<QRect(50,460,75,75);
  }
  void remainingDemoQrCodesMatchGoldens() {
    QFETCH(QString,fixture);QFETCH(QRect,region);
    const auto bytes=corpusFile(fixture);const auto golden=corpusGolden(fixture,0);
    QVERIFY(!bytes.isEmpty());QVERIFY(!golden.isNull());
    const auto result=QtZpl::render(QString::fromLatin1(bytes),{},QtZpl::RenderOptions{
      .width=812,.height=golden.height(),.ignoreLabelHome=true});
    QVERIFY(result.has_value());
    // Both demo labels intentionally draw later fields over the bottom of the
    // QR symbol. Compare the unobscured prefix here; matrix parity below still
    // verifies the complete symbol and automatic mask selection.
    const QRect visibleRegion=fixture==u"barcodes.zpl"
      ? QRect(region.x(),region.y(),region.width(),72)
      : QRect(region.x(),region.y(),region.width(),39);
    qsizetype different=0;
    for(int y=visibleRegion.top();y<=visibleRegion.bottom();++y)
      for(int x=visibleRegion.left();x<=visibleRegion.right();++x)
      different+=(result->labels[0].pixelColor(x,y).value()<128)!=(golden.pixelColor(x,y).value()<128);
    const qsizetype qrCommand=bytes.indexOf("^BQ");const qsizetype field=bytes.indexOf("^FD",qrCommand);
    const qsizetype end=bytes.indexOf("^FS",field);QVERIFY(qrCommand>=0&&field>=0&&end>field);
    QByteArray payload=bytes.sliced(field+3,end-field-3);QVERIFY(payload.startsWith("MM,A"));
    payload.remove(0,4);payload=payload.toUpper();payload.removeIf([](char value){
      return !QByteArrayView{"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:"}.contains(value);
    });
    const int module=fixture==u"barcodes.zpl"?4:3;const int modules=region.width()/module;
    int bestMask=-1;qsizetype bestModuleDifference=std::numeric_limits<qsizetype>::max();
    for(int mask=0;mask<8;++mask){
      const auto matrix=QtZpl::BarcodeEncoders::qrCode(payload,u'M',mask);QVERIFY(matrix.has_value());
      if(matrix->width!=modules)continue;qsizetype moduleDifference=0;
      for(int y=0;y<visibleRegion.height()/module;++y)for(int x=0;x<modules;++x)
        moduleDifference+=matrix->at(x,y)!=(golden.pixelColor(region.x()+x*module+module/2,region.y()+y*module+module/2).value()<128);
      if(moduleDifference<bestModuleDifference){bestModuleDifference=moduleDifference;bestMask=mask;}
    }
    QCOMPARE(bestMask,fixture==u"barcodes.zpl"?4:6);
    const auto expected=QtZpl::BarcodeEncoders::qrCode(payload,u'M',bestMask);QVERIFY(expected.has_value());
    const auto automatic=QtZpl::BarcodeEncoders::qrCode(payload,u'M');QVERIFY(automatic.has_value());
    QCOMPARE(automatic->modules,expected->modules);
    QCOMPARE(bestModuleDifference,0);
    QCOMPARE(different,0);
  }
  void maxiCodeMatchesDemoGoldenCrop() {
    const auto bytes=corpusFile(u"barcodes.zpl");const auto golden=corpusGolden(u"barcodes.zpl",0);
    QVERIFY(!bytes.isEmpty());QVERIFY(!golden.isNull());
    const auto result=QtZpl::render(QString::fromLatin1(bytes),{},QtZpl::RenderOptions{
      .width=812,.height=golden.height(),.ignoreLabelHome=true});
    QVERIFY(result.has_value());
    const QRect region(420,275,210,193);
    const double similarity=inkJaccard(result->labels[0],golden,region);
    QVERIFY2(similarity>=0.80,qPrintable(u"MaxiCode demo crop similarity is %1"_s.arg(similarity,0,'f',6)));
  }
  void font0TextMatchesHelloDemoGolden() {
    const auto bytes=corpusFile(u"hello.zpl");const auto golden=corpusGolden(u"hello.zpl",0);QVERIFY(!bytes.isEmpty());QVERIFY(!golden.isNull());
    const auto result=QtZpl::render(QString::fromLatin1(bytes),{},QtZpl::RenderOptions{.width=812,.height=609,.ignoreLabelHome=true});QVERIFY(result.has_value());
    const QRect region(40,40,730,50);qsizetype different=0;for(int y=region.top();y<=region.bottom();++y)for(int x=region.left();x<=region.right();++x)
      different+=(result->labels[0].pixelColor(x,y).value()<128)!=(golden.pixelColor(x,y).value()<128);
    QCOMPARE(different,0);
  }
  void fontAAndCode128InterpretationTrackLabelary() {
    const auto bytes=corpusFile(u"labelary.zpl");
    const auto golden=corpusGolden(u"labelary.zpl",0);
    QVERIFY(!bytes.isEmpty());QVERIFY(!golden.isNull());
    const auto result=QtZpl::render(QString::fromLatin1(bytes),{},QtZpl::RenderOptions{
      .dpi=203,.width=812,.height=1218,.ignoreLabelHome=true});
    QVERIFY(result.has_value());
    const auto& actual=result->labels.front();
    const double addressSimilarity=inkJaccard(actual,golden,QRect(45,285,535,170));
    const double barcodeSimilarity=inkJaccard(actual,golden,QRect(90,540,670,350));
    QVERIFY2(addressSimilarity>=0.50,qPrintable(u"Font A similarity is %1"_s.arg(addressSimilarity,0,'f',6)));
    QVERIFY2(barcodeSimilarity>=0.98,qPrintable(u"Code 128 interpretation similarity is %1"_s.arg(barcodeSimilarity,0,'f',6)));
  }
  void rotatedTypesetTextUsesRotatedBaseline() {
    const auto result=QtZpl::render(
      u"^XA^PW240^LL240^FT100,100^A0R,30,30^FDTEST^FS^XZ");
    QVERIFY(result.has_value());
    const QRect bounds=inkBounds(result->labels.front());
    QVERIFY(!bounds.isEmpty());
    // R text starts at the ^FT Y coordinate and its vertical baseline is at X.
    QVERIFY2(bounds.left()>=100,qPrintable(u"Rotated glyphs crossed their vertical baseline: %1"_s.arg(bounds.left())));
    QVERIFY2(bounds.top()>=100,qPrintable(u"Rotated field starts above ^FT: %1"_s.arg(bounds.top())));
  }
  void zeroSizedBuiltInFontUsesMatrixDefaults() {
    const auto result=QtZpl::render(
      u"^XA^PW240^LL100^FO20,20^AdN,0,0^FDDEFAULT^FS^XZ");
    QVERIFY(result.has_value());
    const QRect bounds=inkBounds(result->labels.front());
    QVERIFY(!bounds.isEmpty());
    QVERIFY2(bounds.height()>=10,qPrintable(u"Default Font D height is only %1"_s.arg(bounds.height())));
    QVERIFY2(bounds.width()>=30,qPrintable(u"Default Font D width is only %1"_s.arg(bounds.width())));
  }
  void qrCodeMaskMatchesHelloLabelaryVector() {
    const auto golden=corpusGolden(u"hello.zpl",0);QVERIFY(!golden.isNull());
    const QByteArray payload="HTTPS://GITHUB.COM/STIRLINGMARKETINGGROUP/GO-ZPL";
    int bestMask=-1;qsizetype bestDifferent=std::numeric_limits<qsizetype>::max();
    for(int mask=0;mask<8;++mask){
      const auto matrix=QtZpl::BarcodeEncoders::qrCode(payload,u'M',mask);QVERIFY(matrix.has_value());QCOMPARE(matrix->width,29);
      qsizetype different=0;
      for(int y=0;y<29;++y)for(int x=0;x<29;++x)
        different+=matrix->at(x,y)!=(golden.pixelColor(50+x*5+2,110+y*5+2).value()<128);
      if(different<bestDifferent){bestDifferent=different;bestMask=mask;}
    }
    QCOMPARE(bestMask,4);
    QCOMPARE(bestDifferent,0);
    const auto automatic=QtZpl::BarcodeEncoders::qrCode(payload,u'M');QVERIFY(automatic.has_value());
    QCOMPARE(automatic->modules,QtZpl::BarcodeEncoders::qrCode(payload,u'M',4)->modules);
  }
  void rejectsUnsupportedQrModelOne() {
    const auto result=QtZpl::render(
      u"^XA^PW200^LL200^FO10,10^BQ N,1,4^FDMA,MODEL ONE^FS^XZ"_s.remove(u' '));
    QVERIFY(result.has_value());
    QVERIFY(std::any_of(result->diagnostics.cbegin(),result->diagnostics.cend(),[](const auto& diagnostic){
      return diagnostic.code==u"qrcode-model"_s&&diagnostic.severity==QtZpl::Severity::Error;
    }));
    QVERIFY(inkBounds(result->labels.front()).isEmpty());
  }
  void code128AutomaticModeUsesSubsetC() {
    const auto words=QtZpl::BarcodeEncoders::Detail::code128Codewords(u"1234567890",u'A');
    QVERIFY(words.has_value());
    QCOMPARE(*words,QVector<int>({105,12,34,56,78,90,85}));
  }
  void code128HonorsZebraInvocationCodes() {
    const auto words=QtZpl::BarcodeEncoders::Detail::code128Codewords(u">;123456>6AB",u'N');
    QVERIFY(words.has_value());
    QCOMPARE(*words,QVector<int>({105,12,34,56,100,33,34,92}));
  }
  void rendersCode128() {
    auto result=QtZpl::render(u"^XA^PW260^LL100^BY2^FO10,10^BCN,60,N,N,N,N^FDABC123^FS^XZ");
    QVERIFY(result.has_value());
    QVERIFY(std::none_of(result->diagnostics.cbegin(),result->diagnostics.cend(),[](const auto& d){return d.code==u"barcode-render-pending"_s||d.severity==QtZpl::Severity::Error;}));
    QCOMPARE(result->labels[0].pixelColor(10,10),QColor(Qt::black));
    QCOMPARE(result->labels[0].pixelColor(13,10),QColor(Qt::black));
    QCOMPARE(result->labels[0].pixelColor(14,10),QColor(Qt::white));
    QCOMPARE(inkBounds(result->labels[0]),QRect(10,10,202,60));
  }
  void reportsInvalidCode128SubsetC() {
    auto result=QtZpl::render(u"^XA^FO0,0^BCN,50,N,N,N,C^FD123^FS^XZ");
    QVERIFY(result.has_value());
    QVERIFY(std::any_of(result->diagnostics.cbegin(),result->diagnostics.cend(),[](const auto& d){return d.code==u"code128-encode"_s&&d.severity==QtZpl::Severity::Error;}));
    QVERIFY(inkBounds(result->labels[0]).isEmpty());
  }
  void doesNotIgnoreCode128UccCheckDigit() {
    auto result=QtZpl::render(u"^XA^FO0,0^BCN,50,N,N,Y,N^FD0012345678901234567^FS^XZ");
    QVERIFY(result.has_value());
    QVERIFY(std::any_of(result->diagnostics.cbegin(),result->diagnostics.cend(),[](const auto& d){return d.code==u"code128-ucc-check-digit"_s&&d.severity==QtZpl::Severity::Error;}));
    QVERIFY(inkBounds(result->labels[0]).isEmpty());
  }
  void laysOutEan13InterpretationByDigitGroups() {
    auto result=QtZpl::render(u"^XA^PW260^LL180^BY2,3,100^FO30,10^BEN,100,Y,N^FD5901234123457^FS^XZ");
    QVERIFY(result.has_value());
    const auto& image=result->labels.front();
    bool firstDigitLeftOfBars=false;
    for(int y=120;y<150;++y)for(int x=16;x<30;++x)
      firstDigitLeftOfBars|=image.pixelColor(x,y).value()<128;
    QVERIFY(firstDigitLeftOfBars);
    QCOMPARE(image.pixelColor(30,10),QColor(Qt::black));
  }
  void ean13InterpretationTracksLabelaryOcrBGeometry() {
    auto result=QtZpl::render(u"^XA^PW400^LL180^BY3^FO60,10^BEN,110,Y,N^FD5901234123457^FS^XZ");
    QVERIFY(result.has_value());
    const auto& image=result->labels.front();
    QRect firstDigit;
    for(int y=120;y<155;++y)for(int x=20;x<60;++x)
      if(image.pixelColor(x,y).value()<128)firstDigit|=QRect(x,y,1,1);
    QCOMPARE(firstDigit,QRect(29,124,12,21));
  }
  void rendersGs1DataMatrix() {
    auto result=QtZpl::render(u"^XA^PW300^LL300^FO10,10^BXN,4,200,0,0,1,|^FD|10109501101530003|d02917270101|d02910ABC123^FS^XZ");
    QVERIFY(result.has_value());
    QVERIFY(std::none_of(result->diagnostics.cbegin(),result->diagnostics.cend(),[](const auto& d){return d.code==u"barcode-render-pending"_s||d.severity==QtZpl::Severity::Error;}));
    QCOMPARE(result->labels[0].pixelColor(10,10),QColor(Qt::black));
    QCOMPARE(result->labels[0].pixelColor(14,10),QColor(Qt::white));
    QCOMPARE(result->labels[0].pixelColor(10,14),QColor(Qt::black));
  }
  void honorsRequestedDataMatrixSize() {
    auto matrix=QtZpl::BarcodeEncoders::dataMatrix("0109501101020917",true,36);
    QVERIFY(matrix.has_value()); QCOMPARE(matrix->width,36); QCOMPARE(matrix->height,36);
  }
  void gs1UsesGsForInternalSeparators() {
    const QByteArray payload="0109501101020917" + QByteArray(1,char(0x1d)) + "10AB";
    const auto words=QtZpl::BarcodeEncoders::Detail::dataMatrixCodewords(payload,true);
    const char expectedRaw[]{char(232),char(131),char(139),char(180),char(141),char(131),char(132),char(139),char(147),char(30),char(140),char(66),char(67)};
    const QByteArray expected(expectedRaw,sizeof(expectedRaw));
    QCOMPARE(words,expected);
    QCOMPARE(words.count(char(232)),1);
    QCOMPARE(words.count(char(30)),1);
  }
  void reportsInvalidEan13() {
    auto result=QtZpl::render(u"^XA^FO0,0^BEN,50,N^FD5901234123458^FS^XZ");
    QVERIFY(result.has_value());
    QVERIFY(std::any_of(result->diagnostics.cbegin(),result->diagnostics.cend(),[](const auto& d){return d.code==u"ean13-encode"_s;}));
  }
  void dataMatrixMatchesReference() {
    const auto matrix=QtZpl::BarcodeEncoders::dataMatrix(R"({"po":12,"batchAction":"start_end"})",false);
    QVERIFY(matrix.has_value()); QCOMPARE(matrix->width,24); QCOMPARE(matrix->height,24);
    const auto expected=QByteArrayLiteral(
      "#.#.#.#.#.#.#.#.#.#.#.#."
      "#....###..#..#....#...##"
      "##.......#...#.#.#....#."
      "#.###...##..#...##.##..#"
      "##...####..##..#.#.#.##."
      "#.###.##.###..#######.##"
      "#..###...##.##..#.##.##."
      "#.#.#.#.#.#.###....#.#.#"
      "##.#...#.#.#..#...#####."
      "#...####..#...##..#.#..#"
      "##...#...##.###.#.....#."
      "#.###.#.##.#.....###..##"
      "##..#####...#..##...###."
      "###...#.####.##.#.#.#..#"
      "#..###..#.#.####.#.###.."
      "###.#.#..#..#.###.#.##.#"
      "#####.##.###..#.####.#.."
      "#.##.#......#.#..#.#.###"
      "###.#....######.#...##.."
      "##...#..##.###..#...####"
      "#.######.###.##..#...##."
      "#..#..#.##.#..####...#.#"
      "###.###..#..##.#.##...#."
      "########################");
    QCOMPARE(expected.size(),matrix->width*matrix->height);
    for(int y=0;y<matrix->height;++y)for(int x=0;x<matrix->width;++x)
      QCOMPARE(matrix->at(x,y),expected[y*matrix->width+x]=='#');
  }
  void rendersCodeReprintCommandProfile() {
    const auto zpl=u"^XA^CI28^CF0,24"
      "^FO0,0^GB20,20,20^FS^FO5,5^FR^GB10,10,10^FS"
      "^FO25,0^FDПроизведено: 11.08.2026^FS"
      "^FX --- embedded logo ---^FO0,25^GFA,1,1,1,80^FS"
      "^FT10,100^BXN,3,200,36,36,1,|^FD|10109501101530003|d02910ABC123^FS"
      "^FO150,20^BY2^BEN,60,Y,N^FD5901234123457^FS^XZ";
    auto result=QtZpl::render(zpl,{},QtZpl::RenderOptions{.dpi=300,.width=400,.height=240});
    QVERIFY(result.has_value()); QCOMPARE(result->labels.size(),1);
    QVERIFY(std::none_of(result->diagnostics.cbegin(),result->diagnostics.cend(),[](const auto& d){return d.severity==QtZpl::Severity::Error||d.code==u"unsupported-command"_s||d.code==u"barcode-render-pending"_s;}));
    QCOMPARE(result->labels[0].pixelColor(1,1),QColor(Qt::black));
    QCOMPARE(result->labels[0].pixelColor(7,7),QColor(Qt::white));
    QCOMPARE(result->labels[0].pixelColor(0,25),QColor(Qt::black));
  }
  void font0WidthTracksLabelary() {
    auto result=QtZpl::render(u"^XA^PW650^LL100^CI28^CF0,30^FO20,20^FDНоменклатура: Тестовый препарат^FS^XZ");
    QVERIFY(result.has_value());
    const auto& image=result->labels.front();
    int left=image.width(),right=-1;
    for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)
      if(image.pixelColor(x,y).value()<128){left=std::min(left,x);right=std::max(right,x);}
    QVERIFY(right>=left);
    const int renderedWidth=right-left+1;
    const int labelaryWidth=437;
    QVERIFY2(std::abs(renderedWidth-labelaryWidth)<=10,qPrintable(u"Font 0 width %1 differs from Labelary width %2"_s.arg(renderedWidth).arg(labelaryWidth)));
  }
  void font0MatchesStoredLabelaryGolden() {
    const auto zpl=goldenZpl(u"font0");
    const auto golden=goldenImage(u"font0");
    QVERIFY(!zpl.isEmpty());QVERIFY(!golden.isNull());
    const auto result=QtZpl::render(zpl);QVERIFY(result.has_value());
    const double similarity=inkJaccard(result->labels.front(),golden);
    QVERIFY2(similarity>=0.28,qPrintable(u"Font 0 Labelary similarity regressed to %1"_s.arg(similarity,0,'f',6)));
  }
  void ocrBMatchesStoredLabelaryGolden() {
    const auto zpl=goldenZpl(u"ocr-b");
    const auto golden=goldenImage(u"ocr-b");
    QVERIFY(!zpl.isEmpty());QVERIFY(!golden.isNull());
    const auto result=QtZpl::render(zpl);QVERIFY(result.has_value());
    const double similarity=inkJaccard(result->labels.front(),golden,QRect(20,120,40,35));
    QVERIFY2(similarity>=0.35,qPrintable(u"OCR-B Labelary similarity regressed to %1"_s.arg(similarity,0,'f',6)));
  }
  void dataMatrixMatchesStoredLabelaryGolden() {
    const auto zpl=goldenZpl(u"datamatrix");
    const auto golden=goldenImage(u"datamatrix");
    QVERIFY(!zpl.isEmpty());QVERIFY(!golden.isNull());
    const auto result=QtZpl::render(zpl);QVERIFY(result.has_value());
    QCOMPARE(inkJaccard(result->labels.front(),golden),1.0);
  }
  void dataMatrixOrientations_data() {
    QTest::addColumn<QString>("fixture");
    for(const QChar orientation:QStringView{u"NRIB"})
      QTest::newRow(qPrintable(QString{orientation}))<<u"datamatrix-rotation-%1"_s.arg(orientation.toLower());
  }
  void dataMatrixOrientations() {
    QFETCH(QString,fixture);
    const auto zpl=goldenZpl(fixture);const auto golden=goldenImage(fixture);
    QVERIFY(!zpl.isEmpty());QVERIFY(!golden.isNull());
    const auto result=QtZpl::render(zpl);QVERIFY(result.has_value());
    const auto& actual=result->labels.front();
    QCOMPARE(actual.size(),golden.size());
    QCOMPARE(inkBounds(actual),QRect(400,400,90,90));
    QCOMPARE(inkBounds(actual),inkBounds(golden));
    QCOMPARE(inkJaccard(actual,golden),1.0);
  }
  void ean13MatchesStoredLabelaryGolden() {
    const auto zpl=goldenZpl(u"ean13");
    const auto golden=goldenImage(u"ean13");
    QVERIFY(!zpl.isEmpty());QVERIFY(!golden.isNull());
    const auto result=QtZpl::render(zpl);QVERIFY(result.has_value());
    QCOMPARE(inkJaccard(result->labels.front(),golden),1.0);
  }
  void ean13ModuleWidths_data() {
    QTest::addColumn<int>("module");
    QTest::addColumn<QString>("fixture");
    for(int module=1;module<=5;++module)
      QTest::newRow(qPrintable(u"module-%1"_s.arg(module)))<<module<<u"ean-module-%1"_s.arg(module);
  }
  void ean13ModuleWidths() {
    QFETCH(int,module);QFETCH(QString,fixture);
    const auto zpl=goldenZpl(fixture);const auto golden=goldenImage(fixture);
    QVERIFY(!zpl.isEmpty());QVERIFY(!golden.isNull());
    const auto result=QtZpl::render(zpl);QVERIFY(result.has_value());
    const auto& actual=result->labels.front();
    QCOMPARE(actual.size(),golden.size());
    QCOMPARE(inkJaccard(actual,golden,QRect(100,50,95*module,110)),1.0);
    const double similarity=inkJaccard(actual,golden);
    QVERIFY2(similarity>=0.85,qPrintable(u"EAN module %1 Labelary similarity is %2"_s.arg(module).arg(similarity,0,'f',6)));
  }
  void ean13InterpretationPlacement_data() {
    QTest::addColumn<QString>("fixture");
    QTest::newRow("below")<<u"ean-module-3"_s;
    QTest::newRow("above")<<u"ean-above"_s;
  }
  void ean13InterpretationPlacement() {
    QFETCH(QString,fixture);
    const auto zpl=goldenZpl(fixture);const auto golden=goldenImage(fixture);
    QVERIFY(!zpl.isEmpty());QVERIFY(!golden.isNull());
    const auto result=QtZpl::render(zpl);QVERIFY(result.has_value());
    const double similarity=inkJaccard(result->labels.front(),golden);
    QVERIFY2(similarity>=0.90,qPrintable(u"EAN interpretation placement similarity is %1"_s.arg(similarity,0,'f',6)));
  }
  void ean13Orientations_data() {
    QTest::addColumn<QString>("fixture");
    for(const QChar orientation:QStringView{u"NRIB"})
      QTest::newRow(qPrintable(QString{orientation}))<<u"ean-rotation-%1"_s.arg(orientation.toLower());
  }
  void ean13Orientations() {
    QFETCH(QString,fixture);
    const auto zpl=goldenZpl(fixture);const auto golden=goldenImage(fixture);
    QVERIFY(!zpl.isEmpty());QVERIFY(!golden.isNull());
    const auto result=QtZpl::render(zpl);QVERIFY(result.has_value());
    const auto& actual=result->labels.front();
    QCOMPARE(actual.size(),golden.size());
    const QRect actualBounds=inkBounds(actual);const QRect goldenBounds=inkBounds(golden);
    QVERIFY(std::abs(actualBounds.left()-goldenBounds.left())<=1);
    QVERIFY(std::abs(actualBounds.top()-goldenBounds.top())<=1);
    QVERIFY(std::abs(actualBounds.right()-goldenBounds.right())<=1);
    QVERIFY(std::abs(actualBounds.bottom()-goldenBounds.bottom())<=1);
    const double similarity=inkJaccard(actual,golden);
    QVERIFY2(similarity>=0.90,qPrintable(u"EAN orientation Labelary similarity is %1"_s.arg(similarity,0,'f',6)));
  }
  void renderingIsDotInvariantAcrossDpi() {
    const auto zpl=goldenZpl(u"dpi");QVERIFY(!zpl.isEmpty());
    const QList<int> dpis{203,300,600};
    QList<QImage> actual;
    QList<QImage> golden;
    for(const int dpi:dpis){
      const auto result=QtZpl::render(zpl,{},QtZpl::RenderOptions{.dpi=dpi});
      QVERIFY(result.has_value());actual.append(result->labels.front());
      const auto reference=goldenImage(u"dpi-%1"_s.arg(dpi));QVERIFY(!reference.isNull());golden.append(reference);
    }
    QCOMPARE(actual[1],actual[0]);QCOMPARE(actual[2],actual[0]);
    QCOMPARE(golden[1],golden[0]);QCOMPARE(golden[2],golden[0]);
    const double similarity=inkJaccard(actual[0],golden[0]);
    QVERIFY2(similarity>=0.70,qPrintable(u"Combined DPI golden similarity is %1"_s.arg(similarity,0,'f',6)));
  }
};

QTEST_MAIN(QtZplTest)
#include "test_qtzpl.moc"
