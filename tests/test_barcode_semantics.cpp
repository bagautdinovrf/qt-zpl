#include <QtZpl/qtzpl.hpp>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QSet>
#include <QtGui/QImage>
#include <QtTest/QTest>
#include <algorithm>

using namespace Qt::StringLiterals;

namespace {

QString fixture(const QString& name) {
  QFile input(u":/qtzpl/tests/golden/barcode-semantics/"_s + name + u".zpl"_s);
  if (!input.open(QIODevice::ReadOnly)) return {};
  return QString::fromUtf8(input.readAll()).trimmed();
}

QImage rendered(QStringView zpl, QSize size = {609, 406}) {
  const auto result = QtZpl::render(zpl, {}, {.width = size.width(), .height = size.height()});
  if (!result || result->labels.size() != 1) return {};
  return result->labels.front().convertToFormat(QImage::Format_RGB32);
}

// The first barcode field of a one-label document, as analysis resolved it.
std::optional<QtZpl::FieldInfo> barcodeField(QStringView zpl, int occurrence = 0) {
  const auto document = QtZpl::parse(zpl);
  if (!document) return std::nullopt;
  const auto analysis = QtZpl::analyzeLabel(*document, 0);
  if (!analysis) return std::nullopt;
  for (const auto& field : analysis->fields)
    if (field.kind == QtZpl::FieldKind::Barcode && occurrence-- == 0) return field;
  return std::nullopt;
}

std::optional<QtZpl::ResolvedBarcode> resolved(QStringView zpl, int occurrence = 0) {
  const auto field = barcodeField(zpl, occurrence);
  return field ? field->settings.resolvedBarcode : std::nullopt;
}

QStringList diagnosticCodes(const QList<QtZpl::Diagnostic>& diagnostics) {
  QStringList codes;
  for (const auto& diagnostic : diagnostics) codes.append(diagnostic.code);
  return codes;
}

} // namespace

class BarcodeSemanticsTest final : public QObject {
  Q_OBJECT
private slots:
  // Every fixture is an original bitonal Labelary response. The listed names
  // are the open Font 0 contour discrepancies; see the directory README.
  void labelaryParity_data() {
    QTest::addColumn<QString>("name");
    const QDir directory(u":/qtzpl/tests/golden/barcode-semantics"_s);
    const auto names = directory.entryList({u"*.zpl"_s}, QDir::Files, QDir::Name);
    QVERIFY(names.size() >= 60);
    for (const auto& file : names) {
      const auto name = file.chopped(4);
      QTest::newRow(qPrintable(name)) << name;
    }
  }
  void labelaryParity() {
    QFETCH(QString, name);
    static const QSet<QString> openDiscrepancies{
      u"fw-r-text-cf"_s, u"fw-r-text-omitted"_s, u"fw-r-text-explicit-n"_s,
      u"text-explicit-n-then-fw-r"_s, u"text-font-then-fw-r-then-fo"_s, u"fw-r-text-next-field"_s};
    const auto source = fixture(name);
    QVERIFY(!source.isEmpty());
    const QImage expected(u":/qtzpl/tests/golden/barcode-semantics/"_s + name + u"-labelary-bitonal.png"_s);
    QVERIFY(!expected.isNull());
    const auto actual = rendered(source, expected.size());
    QVERIFY(!actual.isNull());
    if (openDiscrepancies.contains(name))
      QEXPECT_FAIL("", "Known Font 0 contour discrepancy; see README.md", Abort);
    QCOMPARE(actual, expected.convertToFormat(QImage::Format_RGB32));
  }

  // Labelary draws lower-case y/n flags exactly like upper-case ones.
  void lowerCaseFlagsMatchUpperCase_data() {
    QTest::addColumn<QString>("lower");
    QTest::addColumn<QString>("upper");
    for (const auto& [lower, upper] : std::initializer_list<std::pair<const char*, const char*>>{
           {"be-lower", "be-upper"}, {"b8-lower-caption", "b8-upper-caption"},
           {"bu-lower-check", "bu-upper-check"}, {"b9-lower-check", "b9-upper-check"},
           {"ba-lower-check", "ba-upper-check"}, {"b2-lower-check", "b2-upper-check"},
           {"bo-lower-flags", "bo-upper-flags"}})
      QTest::newRow(lower) << QString::fromLatin1(lower) << QString::fromLatin1(upper);
  }
  void lowerCaseFlagsMatchUpperCase() {
    QFETCH(QString, lower);
    QFETCH(QString, upper);
    const QImage lowerGolden(u":/qtzpl/tests/golden/barcode-semantics/"_s + lower + u"-labelary-bitonal.png"_s);
    const QImage upperGolden(u":/qtzpl/tests/golden/barcode-semantics/"_s + upper + u"-labelary-bitonal.png"_s);
    QCOMPARE(lowerGolden.convertToFormat(QImage::Format_RGB32), upperGolden.convertToFormat(QImage::Format_RGB32));
    const auto lowerImage = rendered(fixture(lower));
    QVERIFY(!lowerImage.isNull());
    QCOMPARE(lowerImage, rendered(fixture(upper)));
    QCOMPARE(resolved(fixture(lower)).has_value(), resolved(fixture(upper)).has_value());
  }

  // Zebra: a command's own rotation wins; an omitted one follows ^FW.
  void textOrientationFollowsFieldDefaultOnlyWhenOmitted() {
    const auto prefix = u"^XA^PW609^LL406^LH0,0"_s;
    QCOMPARE(rendered(prefix + u"^FWR^FO200,40^A0N,40,40^FDABC^FS^XZ"),
             rendered(prefix + u"^FO200,40^A0N,40,40^FDABC^FS^XZ"));
    QCOMPARE(rendered(prefix + u"^FWR^FO200,40^A0,40,40^FDABC^FS^XZ"),
             rendered(prefix + u"^FO200,40^A0R,40,40^FDABC^FS^XZ"));
    QCOMPARE(rendered(prefix + u"^FWR^CF0,40,40^FO200,40^FDABC^FS^XZ"),
             rendered(prefix + u"^FO200,40^A0R,40,40^FDABC^FS^XZ"));
    QVERIFY(rendered(prefix + u"^FWR^FO200,40^A0N,40,40^FDABC^FS^XZ")
            != rendered(prefix + u"^FWR^FO200,40^A0,40,40^FDABC^FS^XZ"));
    // The later of ^FW and an explicit rotation wins; ^FS restores ^FW.
    QCOMPARE(rendered(prefix + u"^FO200,40^A0N,40,40^FWR^FDABC^FS^XZ"),
             rendered(prefix + u"^FO200,40^A0R,40,40^FDABC^FS^XZ"));
    QCOMPARE(rendered(prefix + u"^A0N,40,40^FWR^FO200,40^FDABC^FS^XZ"),
             rendered(prefix + u"^FO200,40^A0R,40,40^FDABC^FS^XZ"));
    QCOMPARE(rendered(prefix + u"^FWR^FO200,40^A0N,40,40^FDABC^FS^FO400,40^FDDEF^FS^XZ"),
             rendered(prefix + u"^FO200,40^A0N,40,40^FDABC^FS^FO400,40^AAR,9,5^FDDEF^FS^XZ"));
  }

  void scalableFontReportsOmittedOrientationAndExternalFont() {
    const auto document = QtZpl::parse(
      u"^XA^A0,30,20^A0N,30,20^A@R,20,10,E:ARIAL.TTF^CC+"
      u"+A@,20,10+AB+XZ"_s);
    QVERIFY(document.has_value());
    QList<QtZpl::ScalableFont> fonts;
    for (const auto& command : document->labels().front().commands())
      if (const auto* font = std::get_if<QtZpl::ScalableFont>(&command.payload)) fonts.append(*font);
    QCOMPARE(fonts.size(), 5);
    QVERIFY(fonts[0].orientationOmitted);
    QVERIFY(!fonts[0].external);
    QVERIFY(!fonts[1].orientationOmitted);
    QCOMPARE(fonts[2].orientation, QtZpl::Orientation::Rotated90);
    QVERIFY(!fonts[2].orientationOmitted);
    QVERIFY(fonts[2].external);
    QCOMPARE(fonts[2].font, QChar(u'0'));
    QCOMPARE(fonts[2].externalName, u"E:ARIAL.TTF"_s);
    // The ^CC prefix changes the spelling, not the parsed external font.
    QVERIFY(fonts[3].external);
    QVERIFY(fonts[3].orientationOmitted);
    QVERIFY(fonts[3].externalName.isEmpty());
    QCOMPARE(fonts[3].font, QChar(u'0'));
    QCOMPARE(fonts[4].font, QChar(u'B'));
    QVERIFY(!fonts[4].external);

    // Rendering states the substitution; analysis keeps its contract.
    const auto zpl = u"^XA^FO10,10^A@N,20,10,E:ARIAL.TTF^FDX^FS^FO10,40^A0N,20,10^FDY^FS^XZ"_s;
    const auto preview = QtZpl::render(zpl);
    QVERIFY(preview.has_value());
    QCOMPARE(diagnosticCodes(preview->diagnostics), QStringList{u"external-font-substituted"_s});
    QCOMPARE(preview->diagnostics.front().severity, QtZpl::Severity::Warning);
    QCOMPARE(preview->diagnostics.front().offset, zpl.indexOf(u"^FDX"));
    const auto parsed = QtZpl::parse(zpl);
    QVERIFY(QtZpl::analyzeLabel(*parsed, 0)->diagnostics.isEmpty());
  }

  void defaultFontsInheritFieldDirection() {
    const auto document = QtZpl::parse(u"^XA^FWB^FO10,10^FDA^FS^CF0,30^FO10,60^FDB^FS^XZ"_s);
    QVERIFY(document.has_value());
    const auto analysis = QtZpl::analyzeLabel(*document, 0);
    QVERIFY(analysis.has_value());
    QCOMPARE(analysis->fields.size(), 2);
    for (const auto& field : analysis->fields) {
      QVERIFY(field.settings.font.orientationOmitted);
      QVERIFY(field.settings.defaultFont.orientationOmitted);
      QCOMPARE(field.settings.fieldDirection, QtZpl::Orientation::BottomUp);
    }
  }

  void resolvesLinearDefaultsAndBarcodeDefaultHeight() {
    auto code = resolved(u"^XA^BY3,2.5,40^FO10,10^BCN,,Y,N^FD123^FS^XZ");
    QVERIFY(code.has_value());
    QCOMPARE(code->symbology, QtZpl::Symbology::Code128);
    QCOMPARE(code->orientation, QtZpl::Orientation::Normal);
    QVERIFY(!code->orientationOmitted);
    QCOMPARE(code->moduleWidth, 3);
    QCOMPARE(code->wideToNarrowRatio, 2.5);
    QCOMPARE(code->height, 40);
    QVERIFY(code->interpretation);
    QVERIFY(!code->interpretationAbove);
    const auto& options = std::get<QtZpl::Code128Options>(code->options);
    QCOMPARE(options.mode, QChar(u'N'));
    QVERIFY(!options.uccCheckDigit);

    // Labelary clamps a zero ^BY height to one dot; there is no 100-dot default.
    for (const auto* command : {u"^BCN,,N", u"^BEN,,N", u"^B8N,,N", u"^B2N,,N", u"^B3N,N,,N", u"^BKN,N,,N"}) {
      code = resolved(u"^XA^BY2,3,0^FO10,10"_s + QStringView(command) + u"^FD1234567^FS^XZ"_s);
      QVERIFY2(code.has_value(), qPrintable(QString(command)));
      QCOMPARE(code->height, 1);
    }
    code = resolved(u"^XA^BY2^FO10,10^BC^FD123^FS^XZ");
    QVERIFY(code.has_value());
    QCOMPARE(code->height, 10);
    QVERIFY(code->orientationOmitted);
    QVERIFY(code->interpretation);

    code = resolved(u"^XA^FO10,10^BCN,50,Y,,N,U^FD0012345678901234567^FS^XZ");
    QVERIFY(code.has_value());
    QVERIFY(code->interpretationAbove);
    QCOMPARE(std::get<QtZpl::Code128Options>(code->options).mode, QChar(u'U'));
    code = resolved(u"^XA^FO10,10^BCN,50,y,y,y,D^FD123^FS^XZ");
    QVERIFY(code.has_value());
    QVERIFY(code->interpretation);
    QVERIFY(code->interpretationAbove);
    QVERIFY(std::get<QtZpl::Code128Options>(code->options).uccCheckDigit);
  }

  void resolvesCaseInsensitiveFlags() {
    auto code = resolved(u"^XA^FO10,60^B8N,60,y,y^FD1234567^FS^XZ");
    QVERIFY(code.has_value());
    QCOMPARE(code->symbology, QtZpl::Symbology::Ean8);
    QVERIFY(code->interpretation);
    QVERIFY(code->interpretationAbove);
    code = resolved(u"^XA^FO10,60^B8N,60,n^FD1234567^FS^XZ");
    QVERIFY(code.has_value());
    QVERIFY(!code->interpretation);
    // UPC prints its check digit unless e=N; I2of5 and Code 93 add it on e=Y.
    code = resolved(u"^XA^FO10,60^BUN,60,Y,N^FD03600029145^FS^XZ");
    QVERIFY(std::get<QtZpl::LinearOptions>(code->options).checkDigit);
    code = resolved(u"^XA^FO10,60^BUN,60,Y,N,n^FD03600029145^FS^XZ");
    QVERIFY(!std::get<QtZpl::LinearOptions>(code->options).checkDigit);
    code = resolved(u"^XA^FO10,60^B2N,60,Y,N,y^FD1234567^FS^XZ");
    QCOMPARE(code->symbology, QtZpl::Symbology::Interleaved2of5);
    QVERIFY(std::get<QtZpl::LinearOptions>(code->options).checkDigit);
    code = resolved(u"^XA^FO10,60^BAN,60,Y,N,y^FDABC^FS^XZ");
    QVERIFY(std::get<QtZpl::LinearOptions>(code->options).checkDigit);
    code = resolved(u"^XA^FO10,10^BON,4,y,15,y,1^FDABC^FS^XZ");
    QVERIFY(code.has_value());
    const auto& aztec = std::get<QtZpl::AztecOptions>(code->options);
    QVERIFY(aztec.eci);
    QVERIFY(aztec.readerInitialization);
    QCOMPARE(aztec.size, 15);
    QCOMPARE(code->moduleWidth, 4);
    code = resolved(u"^XA^FO10,10^B7N,,2,3,,y^FDABC^FS^XZ");
    QVERIFY(code.has_value());
    QVERIFY(std::get<QtZpl::Pdf417Options>(code->options).truncated);
  }

  void resolvesCode39AndCodabarWithoutDrawnCaption() {
    auto code = resolved(u"^XA^BY2,2.4^FO10,10^B3N,Y,50^FDABC^FS^XZ");
    QVERIFY(code.has_value());
    QCOMPARE(code->symbology, QtZpl::Symbology::Code39);
    QVERIFY(std::get<QtZpl::LinearOptions>(code->options).checkDigit);
    QVERIFY(code->interpretation);
    QCOMPARE(code->wideToNarrowRatio, 2.4);
    code = resolved(u"^XA^BY2,2.4^FO10,10^BKN,N,50,N,N^FD123^FS^XZ");
    QVERIFY(code.has_value());
    QCOMPARE(code->symbology, QtZpl::Symbology::Codabar);
    QCOMPARE(code->wideToNarrowRatio, 2.0);
    QVERIFY(!code->interpretation);

    const auto pending = QtZpl::render(u"^XA^FO10,10^B3N,N,40^FDABC^FS^FO10,80^BKN,N,40,Y^FD1^FS"
                                       u"^FO10,150^B3N,N,40,N^FDABC^FS^XZ");
    QVERIFY(pending.has_value());
    QCOMPARE(diagnosticCodes(pending->diagnostics),
             QStringList({u"barcode-interpretation-pending"_s, u"barcode-interpretation-pending"_s}));
    QCOMPARE(pending->diagnostics.front().severity, QtZpl::Severity::Warning);
    QCOMPARE(pending->diagnostics.front().command, u"^B3N,N,40"_s);
    const auto document = QtZpl::parse(u"^XA^FO10,10^B3N,N,40^FDABC^FS^XZ");
    const auto analysis = QtZpl::analyzeLabel(*document, 0);
    QVERIFY(analysis.has_value());
    QVERIFY(analysis->diagnostics.isEmpty());
  }

  void resolvesMatrixSymbols() {
    auto code = resolved(u"^XA^FO10,10^BXN,5,200,0,0,1,|,2^FD|1ABC^FS^XZ");
    QVERIFY(code.has_value());
    QCOMPARE(code->symbology, QtZpl::Symbology::DataMatrix);
    QCOMPARE(code->moduleWidth, 5);
    QCOMPARE(code->height, 0);
    const auto& matrix = std::get<QtZpl::DataMatrixOptions>(code->options);
    QVERIFY(matrix.gs1);
    QCOMPARE(matrix.format, 1);
    QCOMPARE(matrix.escape, QChar(u'|'));
    QVERIFY(matrix.rectangular);
    QCOMPARE(matrix.quality, 200);
    code = resolved(u"^XA^FO10,10^BXN^FDABC^FS^XZ");
    QCOMPARE(code->moduleWidth, 3);
    QCOMPARE(std::get<QtZpl::DataMatrixOptions>(code->options).escape, QChar(u'~'));

    code = resolved(u"^XA^BY3^FO10,10^B7N,,2,3,4^FDABC^FS^XZ");
    QVERIFY(code.has_value());
    QCOMPARE(code->symbology, QtZpl::Symbology::Pdf417);
    QCOMPARE(code->moduleWidth, 3);
    QCOMPARE(code->height, 9);
    const auto& pdf = std::get<QtZpl::Pdf417Options>(code->options);
    QCOMPARE(pdf.security, 2);
    QCOMPARE(pdf.columns, 3);
    QCOMPARE(pdf.rows, 4);
    QVERIFY(!pdf.truncated);

    // MicroPDF417 mode 3 has 20 rows: the default row height divides ^BY.
    code = resolved(u"^XA^BY2,3,100^FO10,10^BFN,,3^FDABC^FS^XZ");
    QVERIFY(code.has_value());
    QCOMPARE(code->symbology, QtZpl::Symbology::MicroPdf417);
    QCOMPARE(code->height, 5);
    QCOMPARE(std::get<QtZpl::MicroPdf417Options>(code->options).mode, 3);
    QCOMPARE(resolved(u"^XA^BY2,3,30^FO10,10^BFN,,3^FDABC^FS^XZ")->height, 1);
    QCOMPARE(resolved(u"^XA^BY2,3,100^FO10,10^BFN,7,3^FDABC^FS^XZ")->height, 7);

    code = resolved(u"^XA^FO10,10^BRN,2,3,2,40,4^FD12345678901^FS^XZ");
    QVERIFY(code.has_value());
    QCOMPARE(code->symbology, QtZpl::Symbology::DataBar);
    QCOMPARE(code->moduleWidth, 3);
    QCOMPARE(code->height, 40);
    const auto& dataBar = std::get<QtZpl::DataBarOptions>(code->options);
    QCOMPARE(dataBar.type, 2);
    QCOMPARE(dataBar.separatorHeight, 2);
    QCOMPARE(dataBar.segments, 4);
    code = resolved(u"^XA^FO10,10^BRN^FD12345678901^FS^XZ");
    QCOMPARE(code->moduleWidth, 2);
    QCOMPARE(code->height, 25);
    QCOMPARE(std::get<QtZpl::DataBarOptions>(code->options).type, 1);

    code = resolved(u"^XA^FWR^FO10,10^BD4,2,3^FDABC^FS^XZ");
    QVERIFY(code.has_value());
    QCOMPARE(code->symbology, QtZpl::Symbology::MaxiCode);
    QCOMPARE(code->orientation, QtZpl::Orientation::Normal);
    QVERIFY(!code->orientationOmitted);
    const auto& maxi = std::get<QtZpl::MaxiCodeOptions>(code->options);
    QCOMPARE(maxi.mode, 4);
    QCOMPARE(maxi.position, 2);
    QCOMPARE(maxi.total, 3);
  }

  void qrMagnificationSetsFollowingModuleWidth() {
    const auto zpl = u"^XA^BY4^FO10,10^BQN,2,6^FDQA,1^FS^FO10,200^BCN,40^FD1^FS"
                     u"^BY3^FO10,300^BCN,40^FD1^FS^XZ"_s;
    const auto qr = resolved(zpl, 0);
    QVERIFY(qr.has_value());
    QCOMPARE(qr->symbology, QtZpl::Symbology::QrCode);
    QCOMPARE(qr->moduleWidth, 6);
    QCOMPARE(std::get<QtZpl::QrCodeOptions>(qr->options).model, 2);
    QCOMPARE(resolved(zpl, 1)->moduleWidth, 6);
    QCOMPARE(resolved(zpl, 2)->moduleWidth, 3);
    QCOMPARE(resolved(u"^XA^FO10,10^BQN,2,abc^FDQA,1^FS^XZ")->moduleWidth, 2);
    QCOMPARE(resolved(u"^XA^FO10,10^BQN,2,250^FDQA,1^FS^XZ")->moduleWidth, 100);
    // QR modules never rotate; ^FW only moves the ^BY-height anchor margin.
    const auto rotated = resolved(u"^XA^FWB^FO10,10^BQ,2,3^FDQA,1^FS^XZ");
    QCOMPARE(rotated->orientation, QtZpl::Orientation::BottomUp);
    QVERIFY(rotated->orientationOmitted);
  }

  void fieldDirectionAppliesToOmittedBarcodeOrientation() {
    auto code = resolved(u"^XA^FWR^BY2^FO10,10^BC,50^FD123^FS^XZ");
    QVERIFY(code.has_value());
    QCOMPARE(code->orientation, QtZpl::Orientation::Rotated90);
    QVERIFY(code->orientationOmitted);
    code = resolved(u"^XA^FWR^BY2^FO10,10^BCN,50^FD123^FS^XZ");
    QCOMPARE(code->orientation, QtZpl::Orientation::Normal);
    QVERIFY(!code->orientationOmitted);
    code = resolved(u"^XA^FWI^FO10,10^BX,4,200^FDABC^FS^XZ");
    QCOMPARE(code->orientation, QtZpl::Orientation::Inverted);
    // A barcode takes the ^FW in effect at its command, not a later one.
    code = resolved(u"^XA^BY2^FO10,10^BC,60^FWR^FD123^FS^XZ");
    QCOMPARE(code->orientation, QtZpl::Orientation::Normal);
    QVERIFY(code->orientationOmitted);
    const auto document = QtZpl::parse(u"^XA^BC,50^FD1^FS^BCR,50^FD1^FS^BD2^FD1^FS^XZ");
    QVERIFY(document.has_value());
    QList<QtZpl::Barcode> barcodes;
    for (const auto& command : document->labels().front().commands())
      if (const auto* barcode = std::get_if<QtZpl::Barcode>(&command.payload)) barcodes.append(*barcode);
    QCOMPARE(barcodes.size(), 3);
    QVERIFY(barcodes[0].orientationOmitted);
    QVERIFY(!barcodes[1].orientationOmitted);
    // ^BD has no orientation parameter; its first parameter is the mode.
    QVERIFY(!barcodes[2].orientationOmitted);
    QCOMPARE(barcodes[2].orientation, QtZpl::Orientation::Normal);
  }

  void parameterErrorsLeaveAnalysisUnresolvedAndRenderReportsThem_data() {
    QTest::addColumn<QString>("command");
    QTest::addColumn<QString>("code");
    QTest::newRow("datamatrix-number") << u"^BXN,abc"_s << u"barcode-parameter"_s;
    QTest::newRow("datamatrix-quality") << u"^BXN,3,140"_s << u"datamatrix-quality"_s;
    QTest::newRow("datamatrix-ratio") << u"^BXN,3,200,0,0,0,~,3"_s << u"datamatrix-ratio"_s;
    QTest::newRow("qr-model") << u"^BQN,1,3"_s << u"qrcode-model"_s;
    QTest::newRow("pdf417-truncation") << u"^B7N,3,2,3,4,X"_s << u"barcode-parameter"_s;
    QTest::newRow("pdf417-number") << u"^B7N,3,x"_s << u"barcode-parameter"_s;
    QTest::newRow("databar-separator") << u"^BRN,1,2,3"_s << u"barcode-parameter"_s;
    QTest::newRow("aztec-flag") << u"^BON,3,X"_s << u"barcode-parameter"_s;
    QTest::newRow("micropdf-mode") << u"^BFN,3,40"_s << u"barcode-encode"_s;
    QTest::newRow("linear-height") << u"^B8N,0"_s << u"barcode-height"_s;
  }
  void parameterErrorsLeaveAnalysisUnresolvedAndRenderReportsThem() {
    QFETCH(QString, command);
    QFETCH(QString, code);
    const auto zpl = u"^XA^FO10,10"_s + command + u"^FD1234567^FS^XZ"_s;
    const auto field = barcodeField(zpl);
    QVERIFY(field.has_value());
    QVERIFY(field->settings.barcode.has_value());
    QVERIFY(!field->settings.resolvedBarcode.has_value());
    const auto document = QtZpl::parse(zpl);
    QVERIFY(QtZpl::analyzeLabel(*document, 0)->diagnostics.isEmpty());
    const auto result = QtZpl::render(zpl);
    QVERIFY(result.has_value());
    QCOMPARE(diagnosticCodes(result->diagnostics), QStringList{code});
    QCOMPARE(result->diagnostics.front().severity, QtZpl::Severity::Error);
    QCOMPARE(result->diagnostics.front().offset, zpl.indexOf(command.first(3)));
  }

  void extraParametersStayAvailableWithoutChangingResolution() {
    const auto zpl = u"^XA^FO10,10^BCN,40,Y,N,N,N,EXTRA^FD1^FS^XZ"_s;
    const auto field = barcodeField(zpl);
    QVERIFY(field.has_value());
    QCOMPARE(field->settings.barcode->parameters.size(), 7);
    QVERIFY(field->settings.resolvedBarcode.has_value());
    QCOMPARE(field->settings.resolvedBarcode->height, 40);
    QCOMPARE(rendered(zpl), rendered(u"^XA^FO10,10^BCN,40,Y,N,N,N^FD1^FS^XZ"));
  }

  // Labelary takes Codabar start/stop from ^BK k and l in any case, uses A
  // for an invalid one and drops start/stop characters from the field.
  void codabarStartStopComeFromParameters() {
    const auto options = [](QStringView command) {
      const auto code = resolved(u"^XA^FO10,10"_s + command + u"^FD123^FS^XZ"_s);
      return code ? std::get<QtZpl::LinearOptions>(code->options) : QtZpl::LinearOptions{};
    };
    QCOMPARE(options(u"^BKN,N,40,N,N").start, QChar(u'A'));
    QCOMPARE(options(u"^BKN,N,40,N,N").stop, QChar(u'A'));
    QCOMPARE(options(u"^BKN,N,40,N,N,B,c").start, QChar(u'B'));
    QCOMPARE(options(u"^BKN,N,40,N,N,B,c").stop, QChar(u'C'));
    QCOMPARE(options(u"^BKN,N,40,N,N,X,DD").start, QChar(u'A'));
    QCOMPARE(options(u"^BKN,N,40,N,N,X,DD").stop, QChar(u'A'));
    QVERIFY(std::get<QtZpl::LinearOptions>(resolved(u"^XA^FO10,10^BAN,40^FD123^FS^XZ")->options).start.isNull());

    const auto clean = QtZpl::render(u"^XA^FO10,10^BKN,N,40,N,N,B,C^FD123^FS^XZ");
    QVERIFY(clean.has_value());
    QVERIFY(clean->diagnostics.isEmpty());
    const auto zpl = u"^XA^FO10,10^BKN,N,40,N,N,B,C^FDD1x23A^FS^XZ"_s;
    const auto stripped = QtZpl::render(zpl);
    QVERIFY(stripped.has_value());
    QCOMPARE(diagnosticCodes(stripped->diagnostics), QStringList{u"codabar-data-ignored"_s});
    QCOMPARE(stripped->diagnostics.front().severity, QtZpl::Severity::Warning);
    QCOMPARE(stripped->diagnostics.front().offset, zpl.indexOf(u"^BK"));
    QCOMPARE(stripped->labels, clean->labels);
    // Nothing left to encode: Labelary prints no symbol (bk-no-data).
    const auto empty = QtZpl::render(u"^XA^FO10,10^BKN,N,40,N,N^FDAB^FS^XZ");
    QVERIFY(empty.has_value());
    QCOMPARE(diagnosticCodes(empty->diagnostics), QStringList({u"codabar-data-ignored"_s, u"codabar-encode"_s}));
    QCOMPARE(empty->diagnostics.back().severity, QtZpl::Severity::Error);
  }

  // Labelary ignores an Aztec size outside 1-99/101-104/201-232/300,
  // including Zebra's 0, and draws the default symbol.
  void aztecIgnoresInvalidSize() {
    const auto size = [](QStringView value) {
      const auto code = resolved(u"^XA^FO10,10^BON,4,N,"_s + value + u",N^FDABC123^FS^XZ"_s);
      return code ? std::get<QtZpl::AztecOptions>(code->options).size : -1;
    };
    for (const auto* value : {u"", u"0", u"100", u"105", u"150", u"233", u"301", u"-5"})
      QCOMPARE(size(value), 23);
    for (const int value : {1, 99, 101, 104, 201, 232, 300})
      QCOMPARE(size(QString::number(value)), value);
    const auto zero = QtZpl::render(u"^XA^FO10,10^BON,4,N,0,N^FDABC123^FS^XZ");
    QVERIFY(zero.has_value());
    QVERIFY(zero->diagnostics.isEmpty());
    QCOMPARE(zero->labels, QtZpl::render(u"^XA^FO10,10^BON,4,N,,N^FDABC123^FS^XZ")->labels);
  }

  void renderGeometryReportsTheSameResolution() {
    const auto zpl = u"^XA^FWR^BY3,2.5,0^FO40,40^BC,,N^FD123^FS^FO200,40^BFN,,3^FDABC^FS^XZ"_s;
    const auto document = QtZpl::parse(zpl);
    QVERIFY(document.has_value());
    const auto analysis = QtZpl::analyzeLabel(*document, 0);
    const auto preview = QtZpl::renderLabel(*document, 0, {.collectFieldGeometry = true});
    QVERIFY(analysis.has_value());
    QVERIFY(preview.has_value());
    QCOMPARE(analysis->fields.size(), preview->fields.size());
    for (qsizetype i = 0; i < analysis->fields.size(); ++i) {
      const auto& expected = analysis->fields[i].settings.resolvedBarcode;
      const auto& actual = preview->fields[i].field.settings.resolvedBarcode;
      QVERIFY(expected.has_value());
      QVERIFY(actual.has_value());
      QCOMPARE(actual->symbology, expected->symbology);
      QCOMPARE(actual->orientation, expected->orientation);
      QCOMPARE(actual->moduleWidth, expected->moduleWidth);
      QCOMPARE(actual->height, expected->height);
    }
    // Rotated by ^FW, one-dot bars from the clamped ^BY height.
    QCOMPARE(preview->fields.front().paintBounds.width(), 1);
  }
};

QTEST_MAIN(BarcodeSemanticsTest)
#include "test_barcode_semantics.moc"
