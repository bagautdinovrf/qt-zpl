#include <QtTest/QTest>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtZpl/qtzpl.hpp>
#include "../src/linear_encoders.hpp"

using namespace Qt::StringLiterals;

class CommandExtensionsTest final : public QObject {
  Q_OBJECT
private slots:
  void fieldCoordinatesDoNotWrapIntoTheLabel() {
    const auto result=QtZpl::render(u"^XA^PW40^LL40^LH2,0^LS2147483647^FO2147483647,10^GB2,2,2^FS^XZ");
    QVERIFY(result);QVERIFY(result->diagnostics.isEmpty());
    QImage blank(40,40,QImage::Format_ARGB32_Premultiplied);blank.fill(Qt::white);
    QCOMPARE(result->labels.front(),blank);
  }
  void unicodeHexFields() {
    const auto document=QtZpl::parse(u"^XA^CI28^FH^FD_D0_81_D1_91 _E2_80_94 _E2_88_92 _E2_84_96^FS"
      u"^FH!^FV!C2!A1 Ж!E2!82!AC^FS^FH^FS^FD_41^FS^CI0^FH^FD_FF^FS^XZ");
    QVERIFY(document);QVERIFY(document->diagnostics().isEmpty());
    QStringList fields;
    for(const auto& command:document->labels().front().commands())
      if(const auto* field=std::get_if<QtZpl::FieldData>(&command.payload))fields.append(field->data);
    QCOMPARE(fields,(QStringList{u"Ёё — − №"_s,u"¡ Ж€"_s,u"_41"_s,QString(QChar(0xff))}));
    const auto invalid=QtZpl::parse(u"^XA^CI28^FH^FD_D0_FF^FS^XZ");
    QVERIFY(invalid);QCOMPARE(invalid->diagnostics().size(),1);
    QCOMPARE(invalid->diagnostics().front().code,u"invalid-field-encoding");
    QCOMPARE(invalid->diagnostics().front().command,u"^FD_D0_FF");
  }
  void linearKnownVectors() {
    using namespace QtZpl::BarcodeEncoders::Linear;
    auto ean=encode(u"B8",u"9638507");QVERIFY(ean);
    QCOMPARE(ean->text,u"96385074");
    QCOMPARE(ean->modules.size(),67);
    QByteArray bits;for(bool bit:ean->modules)bits+=bit?'1':'0';
    QCOMPARE(bits,QByteArray("1010001011010111101111010110111010101001110111001010001001011100101"));
    auto upce=encode(u"B9",u"0425261");QVERIFY(upce);QCOMPARE(upce->text,u"04252614");QCOMPARE(upce->modules.size(),51);
    QVERIFY(!encode(u"B8",u"96385075"));QVERIFY(!encode(u"BU",u"036000291453"));
    QVERIFY(!encode(u"B2",u"12A4"));QVERIFY(!encode(u"BI",u"12\u06614"));
    QVERIFY(!encode(u"BA",u"\u0410"));
  }
  void parserRecognizesExtensions_data() {
    QTest::addColumn<QString>("command");
    for(const auto code:{u"B8",u"B9",u"BA",u"BR",u"BF",u"BU",u"B2",u"BO",u"B0",u"BI",u"LT",u"PM",u"LR",u"FP"})
      for(const auto suffix:{u"",u",,,",u"N,3,2",u"invalid"})
        QTest::newRow(qPrintable(QString(code)+suffix))<<u'^'+QString(code)+suffix;
  }
  void parserRecognizesExtensions() {
    QFETCH(QString,command);const auto doc=QtZpl::parse(u"^XA"_s+command+u"^XZ"_s);QVERIFY(doc);
    for(const auto& d:doc->diagnostics())QVERIFY2(d.code!=u"unsupported-command",qPrintable(command));
    QVERIFY(doc->labels().front().commands().size()>=3);
  }
  void fieldPositionParameters() {
    const auto document=QtZpl::parse(u"^XA^FWB,1^FO20,30,2^FT,40,1^FT^FOinvalid,,invalid^FPV,5^FS^XZ");
    QVERIFY(document);
    const auto& commands=document->labels().front().commands();
    const auto& direction=std::get<QtZpl::FieldDirection>(commands[1].payload);
    QCOMPARE(direction.orientation,QtZpl::Orientation::BottomUp);
    QCOMPARE(direction.justification,QtZpl::Justification::Right);
    const auto& origin=std::get<QtZpl::FieldOrigin>(commands[2].payload);
    QCOMPARE(origin.x,20);QCOMPARE(origin.y,30);QCOMPARE(origin.justification,QtZpl::Justification::Auto);
    const auto& partial=std::get<QtZpl::FieldTypeset>(commands[3].payload);
    QVERIFY(partial.usePreviousX);QVERIFY(!partial.usePreviousY);QCOMPARE(partial.y,40);
    QCOMPARE(partial.justification,QtZpl::Justification::Right);
    const auto& omitted=std::get<QtZpl::FieldTypeset>(commands[4].payload);
    QVERIFY(omitted.usePreviousX&&omitted.usePreviousY&&omitted.useDefaultJustification);
    const auto& invalid=std::get<QtZpl::FieldOrigin>(commands[5].payload);
    QCOMPARE(invalid.x,0);QCOMPARE(invalid.y,0);QCOMPARE(invalid.justification,QtZpl::Justification::Left);
    const auto& parameter=std::get<QtZpl::FieldParameter>(commands[6].payload);
    QCOMPARE(parameter.direction,u'V');QCOMPARE(parameter.spacing,5);
  }
  void labelaryGoldens_data() {
    QTest::addColumn<QString>("name");
    const QDir directory(u":/qtzpl/extensions"_s);
    for(const auto& file:directory.entryList({u"*.zpl"_s},QDir::Files))
      QTest::newRow(qPrintable(file))<<file.chopped(4);
  }
  void labelaryGoldens() {
    QFETCH(QString,name);QFile file(u":/qtzpl/extensions/"_s+name+u".zpl"_s);QVERIFY(file.open(QIODevice::ReadOnly));
    const QImage expected(u":/qtzpl/extensions/"_s+name+u"-labelary-bitonal.png"_s);QVERIFY(!expected.isNull());
    const auto result=QtZpl::render(QString::fromUtf8(file.readAll()),{},QtZpl::RenderOptions{.width=expected.width(),.height=expected.height()});QVERIFY(result);
    QVERIFY2(result->diagnostics.isEmpty(),result->diagnostics.isEmpty()?"":qPrintable(result->diagnostics.front().message));
    const auto actual=result->labels.front();QCOMPARE(actual.size(),expected.size());
    qsizetype differences=0;
    for(int y=0;y<actual.height();++y)for(int x=0;x<actual.width();++x)
      differences+=(qGray(actual.pixel(x,y))<128)!=(qGray(expected.pixel(x,y))<128);
    if(differences){actual.save(QCoreApplication::applicationDirPath()+u"/extension-"_s+name+u"-actual.png"_s);}
    QCOMPARE(differences,0);
  }
  void transformsAndFieldLifetime() {
    const QString body=u"^FO10,20^GB30,15,15^FS^FO80,40^GB10,30,30^FS"_s;
    const auto render=[&](const QString& commands){return QtZpl::render(u"^XA^PW140^LL120"_s+commands+u"^XZ"_s);};
    auto normal=render(body),mirror=render(u"^PMY"_s+body),shift=render(u"^LT20"_s+body);
    QVERIFY(normal&&mirror&&shift);
    QCOMPARE(mirror->labels.front(),normal->labels.front().flipped(Qt::Horizontal));
    const auto& original=normal->labels.front();const auto& moved=shift->labels.front();
    for(int y=0;y<100;++y)for(int x=0;x<140;++x)QCOMPARE(moved.pixel(x,y+20),original.pixel(x,y));
    auto reverse=render(u"^LRY^FO10,20^GB30,15,15^FS^FO10,20^GB30,15,15^FS^LRN^FO80,40^GB10,30,30^FS"_s);
    QVERIFY(reverse);QCOMPARE(reverse->labels.front().pixelColor(15,25),QColor(Qt::white));QCOMPARE(reverse->labels.front().pixelColor(85,45),QColor(Qt::black));
  }
  void boundedTextAndRatio() {
    for(const auto ratio:{u"1e200",u"inf",u"1",u"4"}){
      const auto result=QtZpl::render(u"^XA^PW200^LL100^BY3,"_s+ratio+u"^B2N,50,N,N^FD12345678^FS^XZ"_s);
      QVERIFY(result);QVERIFY(!result->diagnostics.isEmpty());
      QCOMPARE(result->diagnostics.front().code,u"barcode-ratio"_s);
      QVERIFY(result->diagnostics.front().offset>=0);
      QVERIFY(result->diagnostics.front().command.startsWith(u"^B2"));
      QImage blank(200,100,QImage::Format_ARGB32_Premultiplied);blank.fill(Qt::white);
      QCOMPARE(result->labels.front(),blank);
    }
    const auto result=QtZpl::render(u"^XA^PW100^LL100^AAN,2147483647,2147483647^FPH,9999^FDABC^FS^XZ");
    QVERIFY(result);QVERIFY(!result->diagnostics.isEmpty());QCOMPARE(result->diagnostics.front().code,u"text-size"_s);
    const auto crowded=QtZpl::render(u"^XA^PW100^LL100^AAN,27,15^FPH,9999^FD"_s+QString(300000,u'A')+u"^FS^XZ"_s);
    QVERIFY(crowded);QVERIFY(!crowded->diagnostics.isEmpty());QCOMPARE(crowded->diagnostics.front().code,u"text-size"_s);
  }
  void barcodesReverseClipAndDotGeometry() {
    const QStringList fields{
      u"^B8N,60,N,N^FD9638507"_s,u"^B9N,60,N,N^FD0425261"_s,
      u"^BUN,60,N,N^FD03600029145"_s,u"^BAN,60,N,N^FDABC-123"_s,
      u"^B2N,60,N,N^FD12345678"_s,u"^BIN,60,N,N^FD12345678"_s,
      u"^BON,3,N,23,N^FDABC123"_s,u"^BRN,1,2^FD0950110153000"_s,
      u"^BFN,3,7^FDABCDEFG"_s,u"^BXN,3,200,26,12,6,~,2^FD123456"_s,
      u"^B7N,3,2,3,5,Y^FDABC"_s,u"^BD4^FDABC123"_s
    };
    for(const auto& field:fields){
      const QString body=u"^BY2^FO20,25"_s+field+u"^FS"_s;
      const QString label=u"^XA^PW180^LL120"_s+body+u"^XZ"_s;
      const auto normal=QtZpl::render(label);QVERIFY(normal);QVERIFY(normal->diagnostics.isEmpty());
      const auto inverted=QtZpl::render(u"^XA^PW180^LL120^FO0,0^GB180,120,120^FS^LRY"_s+body+u"^XZ"_s);
      QVERIFY(inverted);QVERIFY(inverted->diagnostics.isEmpty());
      QImage inverse=normal->labels.front();inverse.invertPixels(QImage::InvertRgb);
      QCOMPARE(inverted->labels.front(),inverse);
      const auto large=QtZpl::render(label,{},QtZpl::RenderOptions{.width=600,.height=400});QVERIFY(large);
      QCOMPARE(normal->labels.front(),large->labels.front().copy((600-180)/2,0,180,120));
      const auto highDpi=QtZpl::render(label,{},QtZpl::RenderOptions{.dpi=600});QVERIFY(highDpi);
      QCOMPARE(normal->labels.front(),highDpi->labels.front());
    }
  }
  void invalidBarcodes_data() {
    QTest::addColumn<QString>("command");QTest::addColumn<QString>("data");QTest::addColumn<QString>("code");
    for(const int quality:{0,50,80,100,140})
      QTest::newRow(qPrintable(u"legacy-ecc-%1"_s.arg(quality)))<<u"^BXN,3,%1"_s.arg(quality)<<u"ABC"_s<<u"datamatrix-quality"_s;
    QTest::newRow("dm-ratio")<<u"^BXN,3,200,,,6,~,3"_s<<u"ABC"_s<<u"datamatrix-ratio"_s;
    QTest::newRow("dm-size-overflow")<<u"^BXN,2147483647,200"_s<<u"ABC"_s<<u"barcode-size"_s;
    QTest::newRow("dm-unicode")<<u"^BXN,3,200"_s<<u"\u0410"_s<<u"datamatrix-encoding"_s;
    QTest::newRow("dm-long")<<u"^BXN,3,200"_s<<QString(16385,u'A')<<u"datamatrix-encode"_s;
    QTest::newRow("dm-invalid-number")<<u"^BXN,3,200,not-a-number"_s<<u"ABC"_s<<u"barcode-parameter"_s;
    QTest::newRow("pdf-grid-overflow")<<u"^B7N,3,0,30,90,Y"_s<<u"ABC"_s<<u"pdf417-encode"_s;
    QTest::newRow("pdf-size-overflow")<<u"^B7N,2147483647,2,2,8,Y"_s<<u"ABC"_s<<u"barcode-size"_s;
    QTest::newRow("pdf-negative-height")<<u"^B7N,-1,2,2,8,Y"_s<<u"ABC"_s<<u"barcode-size"_s;
    QTest::newRow("pdf-unicode")<<u"^B7N,3,2,2,8,Y"_s<<u"\u0410"_s<<u"pdf417-encoding"_s;
    QTest::newRow("pdf-long")<<u"^B7N,3,0,,,Y"_s<<QString(2711,u'1')<<u"pdf417-encode"_s;
    QTest::newRow("pdf-invalid-flag")<<u"^B7N,3,2,2,8,invalid"_s<<u"ABC"_s<<u"barcode-parameter"_s;
    QTest::newRow("micro-invalid-mode")<<u"^BFN,3,34"_s<<u"ABC"_s<<u"barcode-encode"_s;
    QTest::newRow("micro-invalid-number")<<u"^BFN,3,bad"_s<<u"ABC"_s<<u"barcode-parameter"_s;
    QTest::newRow("micro-unicode")<<u"^BFN,3,0"_s<<u"\u0410"_s<<u"barcode-encoding"_s;
    QTest::newRow("micro-oversized-height")<<u"^BFN,10000,0"_s<<u"ABC"_s<<u"barcode-size"_s;
    QTest::newRow("aztec-invalid-flag")<<u"^BON,3,invalid"_s<<u"ABC"_s<<u"barcode-parameter"_s;
    QTest::newRow("aztec-eci")<<u"^BON,3,Y"_s<<u"ABC"_s<<u"barcode-encode"_s;
    QTest::newRow("aztec-append")<<u"^BON,3,N,23,N,2"_s<<u"ABC"_s<<u"barcode-encode"_s;
    QTest::newRow("aztec-size")<<u"^BON,2147483647"_s<<u"ABC"_s<<u"barcode-size"_s;
    QTest::newRow("databar-height")<<u"^BRN,1,2,1,-1"_s<<u"1234567890123"_s<<u"barcode-parameter"_s;
    QTest::newRow("databar-separator")<<u"^BRN,1,2,3"_s<<u"1234567890123"_s<<u"barcode-parameter"_s;
    QTest::newRow("databar-segments")<<u"^BRN,6,2,1,25,3"_s<<u"(01)12345678901231"_s<<u"barcode-encode"_s;
  }
  void invalidBarcodes() {
    QFETCH(QString,command);QFETCH(QString,data);QFETCH(QString,code);
    const QString prefix=u"^XA^PW120^LL120^FO10,10"_s;
    const auto result=QtZpl::render(prefix+command+u"^FD"_s+data+u"^FS^XZ"_s);
    QVERIFY(result);QCOMPARE(result->labels.size(),1);QCOMPARE(result->diagnostics.size(),1);
    const auto& diagnostic=result->diagnostics.front();
    QCOMPARE(diagnostic.severity,QtZpl::Severity::Error);QCOMPARE(diagnostic.code,code);
    QCOMPARE(diagnostic.offset,prefix.size());QCOMPARE(diagnostic.command,command);
    QImage blank(120,120,QImage::Format_ARGB32_Premultiplied);blank.fill(Qt::white);
    QCOMPARE(result->labels.front(),blank);
  }
};
QTEST_MAIN(CommandExtensionsTest)
#include "test_command_extensions.moc"
