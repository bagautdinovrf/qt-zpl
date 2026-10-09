#include <QtTest/QTest>
#include <QtZpl/qtzpl.hpp>

#include <algorithm>
#include <future>
#include <stop_token>

namespace {
using namespace Qt::StringLiterals;

QRect inkBounds(const QImage& image) {
  QRect bounds;
  for (int y = 0; y < image.height(); ++y)
    for (int x = 0; x < image.width(); ++x)
      if (image.pixelColor(x, y) != QColor(Qt::white)) bounds |= QRect(x, y, 1, 1);
  return bounds;
}

void compareDiagnostics(const QList<QtZpl::Diagnostic>& actual,
                        const QList<QtZpl::Diagnostic>& expected) {
  QCOMPARE(actual.size(), expected.size());
  for (qsizetype i = 0; i < actual.size(); ++i) {
    QCOMPARE(actual[i].severity, expected[i].severity);
    QCOMPARE(actual[i].code, expected[i].code);
    QCOMPARE(actual[i].message, expected[i].message);
    QCOMPARE(actual[i].offset, expected[i].offset);
    QCOMPARE(actual[i].command, expected[i].command);
    QVERIFY(actual[i].labelIndex == expected[i].labelIndex);
    QVERIFY(actual[i].fieldId == expected[i].fieldId);
    QVERIFY(actual[i].sourceSpan == expected[i].sourceSpan);
  }
}

void compareFieldIdentity(const QtZpl::FieldInfo& actual, const QtZpl::FieldInfo& expected) {
  QCOMPARE(actual.id, expected.id);
  QCOMPARE(actual.labelIndex, expected.labelIndex);
  QCOMPARE(actual.commandIndex, expected.commandIndex);
  QVERIFY(actual.sourceSpan == expected.sourceSpan);
  QVERIFY(actual.payloadSpan == expected.payloadSpan);
  QCOMPARE(actual.kind, expected.kind);
  QCOMPARE(actual.hasUnknownCommands, expected.hasUnknownCommands);
  QCOMPARE(actual.settings.position, expected.settings.position);
  QCOMPARE(actual.settings.baseline, expected.settings.baseline);
  QCOMPARE(actual.settings.reverse, expected.settings.reverse);
  QCOMPARE(actual.settings.characterSet, expected.settings.characterSet);
  QCOMPARE(actual.settings.hexIndicator, expected.settings.hexIndicator);
}

QtZpl::GraphicField graphicFrom(QStringView source) {
  const auto document = QtZpl::parse(source);
  if (document)
    for (const auto& label : document->labels())
      for (const auto& command : label.commands())
        if (const auto* graphic = std::get_if<QtZpl::GraphicField>(&command.payload)) return *graphic;
  return {};
}
}

// All scenarios deliberately consume only the installed public headers.
class DesignerApiTest final : public QObject {
  Q_OBJECT

private slots:
  void concurrentFirstAndRepeatedCalls() {
    const auto document = QtZpl::parse(
      u"^XA^PW220^LL160^CI28^FO10,10^A0N,20,14^FDЖé^FS^XZ"
      u"^XA^PW220^LL160^FO10,10^BXN,2,200^FH^FD_D0_96_C3_A9^FS"
      u"^FT10,110^A0N,20,14^FDtext^FS^XZ");
    QVERIFY(document.has_value());
    const QtZpl::RenderOptions options{.collectFieldGeometry = true};
    const auto run = [&] {
      return std::pair{QtZpl::renderLabel(*document, 1, options),
                       QtZpl::analyzeLabel(*document, 1, options)};
    };
    // No rendering/analysis call precedes these concurrent initial calls.
    auto first = std::async(std::launch::async, run);
    auto second = std::async(std::launch::async, run);
    auto a = first.get();
    auto b = second.get();
    QVERIFY(a.first.has_value());
    QVERIFY(a.second.has_value());
    QVERIFY(b.first.has_value());
    QVERIFY(b.second.has_value());
    QCOMPARE(a.first->image, b.first->image);
    compareDiagnostics(a.first->diagnostics, b.first->diagnostics);
    QCOMPARE(a.first->fields.size(), a.second->fields.size());
    QCOMPARE(a.second->fields.size(), b.second->fields.size());
    for (qsizetype i = 0; i < a.second->fields.size(); ++i) {
      compareFieldIdentity(a.first->fields[i].field, a.second->fields[i]);
      compareFieldIdentity(a.second->fields[i], b.second->fields[i]);
      QCOMPARE(a.first->fields[i].paintBounds, b.first->fields[i].paintBounds);
    }
    const QImage original = b.first->image;
    a.first->image.fill(Qt::red);
    QCOMPARE(b.first->image, original);
    const auto repeated = run();
    QVERIFY(repeated.first.has_value());
    QCOMPARE(repeated.first->image, original);
    compareDiagnostics(repeated.first->diagnostics, b.first->diagnostics);
  }

  void selectedLabelsMatchFullDocument() {
    const auto document = QtZpl::parse(
      u"preamble\r\n^XA^PW180^LL130^LH7,9^CI28^FWN^BY2,2,32"
      u"^FO15,10^BXN,2,200^FDЖé^FS^FT20,110^A0N,18,12^FDone^FS^XZ\r\n"
      u"^XA^PW180^LL130^PMY^POI^FO12,15^BXN,2,200^FH^FD_D0_96_C3_A9^FS"
      u"^FO20,80^GB35,20,2^FS^XZ"
      u"^XA^PW180^LL130^FT30,100^A0R,18,12^FB60,2,1,C^FDa\\&b^FS^XZ");
    QVERIFY(document.has_value());
    QCOMPARE(document->labels().size(), 3);
    for (const auto options : {QtZpl::RenderOptions{},
                              QtZpl::RenderOptions{.dpi = 600, .width = 200, .height = 150,
                                                   .ignoreLabelHome = true, .collectFieldGeometry = true}}) {
      const auto full = QtZpl::render(*document, options);
      const auto analysis = QtZpl::analyze(*document, options);
      QVERIFY(full.has_value());
      QVERIFY(analysis.has_value());
      QCOMPARE(full->labels.size(), 3);
      for (int page = 0; page < 3; ++page) {
        const auto selected = QtZpl::renderLabel(*document, page, options);
        const auto selectedAnalysis = QtZpl::analyzeLabel(*document, page, options);
        QVERIFY(selected.has_value());
        QVERIFY(selectedAnalysis.has_value());
        QCOMPARE(selected->labelIndex, page);
        QCOMPARE(selected->image, full->labels[page]);
        compareDiagnostics(selected->diagnostics, document->diagnostics());
        QList<QtZpl::FieldInfo> expectedFields;
        for (const auto& field : analysis->fields)
          if (field.labelIndex == page) expectedFields.append(field);
        QCOMPARE(selectedAnalysis->fields.size(), expectedFields.size());
        for (qsizetype i = 0; i < expectedFields.size(); ++i)
          compareFieldIdentity(selectedAnalysis->fields[i], expectedFields[i]);
        QCOMPARE(selected->fields.size(), options.collectFieldGeometry ? expectedFields.size() : 0);
        for (const auto& field : selected->fields) QCOMPARE(field.field.labelIndex, page);
      }
    }
  }

  void selectedLabelSkipsUnselectedCanvases() {
    const auto document = QtZpl::parse(
      u"^XA^PW2147483647^LL2147483647^XZ"
      u"^XA^PW32^LL24^FO3,4^GB8,6,6^FS^XZ"
      u"^XA^PW32000^LL32000^XZ");
    QVERIFY(document.has_value());
    QVERIFY(!QtZpl::render(*document).has_value());
    const auto selected = QtZpl::renderLabel(*document, 1, {.maxTotalPixels = 768});
    QVERIFY(selected.has_value());
    QCOMPARE(selected->image.size(), QSize(32, 24));
    QCOMPARE(inkBounds(selected->image), QRect(3, 4, 8, 6));
  }

  void diagnosticsKeepDocumentPositionsAndFieldIdentity() {
    const QString source =
      u"^XA^PW120^LL80^ZZunhandled^FS^XZ\r\n"
      u"^XA^PW120^LL80^FO3,4^BCN,30,N,N,N,C^FDABC^FS^XZ"_s;
    const auto document = QtZpl::parse(source);
    QVERIFY(document.has_value());
    QCOMPARE(document->diagnostics().size(), 1);
    const auto first = QtZpl::renderLabel(*document, 0);
    const auto second = QtZpl::renderLabel(*document, 1, {.collectFieldGeometry = true});
    const auto repeated = QtZpl::renderLabel(*document, 1, {.collectFieldGeometry = true});
    QVERIFY(first.has_value());
    QVERIFY(second.has_value());
    QVERIFY(repeated.has_value());
    compareDiagnostics(first->diagnostics, document->diagnostics());
    QCOMPARE(second->diagnostics.size(), 2);
    compareDiagnostics(second->diagnostics, repeated->diagnostics);
    const auto& error = second->diagnostics.back();
    QCOMPARE(error.code, u"code128-encode"_s);
    QCOMPARE(error.severity, QtZpl::Severity::Error);
    QCOMPARE(error.offset, source.indexOf(u"^BC"));
    QVERIFY(error.labelIndex == std::optional<int>{1});
    QVERIFY(error.fieldId == std::optional<qsizetype>{source.indexOf(u"^FD")});
    QVERIFY(error.sourceSpan.has_value());
    QVERIFY(document->sourceText(*error.sourceSpan).startsWith(u"^BC"));
    QCOMPARE(second->fields.size(), 1);
    QCOMPARE(second->fields.front().status, QtZpl::FieldStatus::Error);
    QVERIFY(inkBounds(second->image).isEmpty());
    // Rendering does not add diagnostics to the immutable parsed snapshot.
    QCOMPARE(document->diagnostics().size(), 1);
  }

  void analysisExposesEffectiveStateAndFieldLifetime() {
    const QString source =
      u"^XA^PW300^LL180^LH7,9^LT3^LS4^CFA,18,10^FWR,1^BY3,2,44^CI28"
      u"^FO20,30^A0B,24,12^FB80,2,1,C^FPH,2^FR^FH!^FDx!D0!96^FS"
      u"^FT50,100^BCN,,N^FD1234^FS^XZ"_s;
    const auto document = QtZpl::parse(source);
    QVERIFY(document.has_value());
    const auto analysis = QtZpl::analyze(*document, {.maxTotalPixels = 1});
    QVERIFY(analysis.has_value());
    QVERIFY(analysis->diagnostics.isEmpty());
    QCOMPARE(analysis->fields.size(), 2);
    const auto& text = analysis->fields[0];
    QCOMPARE(text.kind, QtZpl::FieldKind::Text);
    QCOMPARE(text.id, source.indexOf(u"^FD"));
    QCOMPARE(document->sourceText(text.payloadSpan), u"^FDx!D0!96");
    const auto rawField = document->sourceText(text.sourceSpan);
    QVERIFY(rawField.contains(u"^FO20,30"));
    QVERIFY(rawField.endsWith(u"^FS"));
    const auto& state = text.settings;
    QCOMPARE(state.position, QPoint(31, 42));
    QVERIFY(!state.baseline);
    QCOMPARE(state.justification, QtZpl::Justification::Right);
    QCOMPARE(state.font.font, QChar(u'0'));
    QCOMPARE(state.font.orientation, QtZpl::Orientation::BottomUp);
    QCOMPARE(state.font.height, 24);
    QCOMPARE(state.font.width, 12);
    QCOMPARE(state.defaultFont.font, QChar(u'A'));
    QCOMPARE(state.defaultFont.height, 18);
    QCOMPARE(state.defaultFont.width, 10);
    QCOMPARE(state.fieldDirection, QtZpl::Orientation::Rotated90);
    QCOMPARE(state.barcodeDefaults.moduleWidth, 3);
    QCOMPARE(state.barcodeDefaults.wideToNarrowRatio, 2.0);
    QCOMPARE(state.barcodeDefaults.height, 44);
    QVERIFY(state.block.has_value());
    QCOMPARE(state.block->width, 80);
    QCOMPARE(state.block->justification, QtZpl::Justification::Center);
    QCOMPARE(state.parameter.spacing, 2);
    QCOMPARE(state.characterSet, 28);
    QCOMPARE(state.hexIndicator, QChar(u'!'));
    QCOMPARE(state.labelHome, QPoint(7, 9));
    QCOMPARE(state.labelShift, 4);
    QCOMPARE(state.labelTop, 3);
    QVERIFY(state.reverse);
    const auto& barcode = analysis->fields[1];
    QCOMPARE(barcode.kind, QtZpl::FieldKind::Barcode);
    QCOMPARE(barcode.settings.position, QPoint(61, 112));
    QVERIFY(barcode.settings.baseline);
    QCOMPARE(barcode.settings.font.font, QChar(u'A'));
    QCOMPARE(barcode.settings.parameter.spacing, 0);
    QVERIFY(!barcode.settings.block.has_value());
    QVERIFY(!barcode.settings.reverse);
    QVERIFY(barcode.settings.hexIndicator.isNull());
    QVERIFY(barcode.settings.barcode.has_value());
    QCOMPARE(barcode.settings.barcode->symbology, u"BC"_s);
    QCOMPARE(barcode.settings.barcodeDefaults.moduleWidth, 3);
    QCOMPARE(barcode.settings.characterSet, 28);
  }

  void analysisFlagsUnknownCommandsWithoutRasterizing() {
    for (const bool preserve : {true, false}) {
      const auto document = QtZpl::parse(
        u"^XA^PW300^LL180^FO10,20^ZZfuture^FDtext^FS^XZ", {.preserveUnknownCommands = preserve});
      QVERIFY(document.has_value());
      const auto analysis = QtZpl::analyze(*document, {.maxTotalPixels = 1});
      QVERIFY(analysis.has_value());
      compareDiagnostics(analysis->diagnostics, document->diagnostics());
      const auto text = std::ranges::find_if(analysis->fields, [](const auto& field) {
        return field.kind == QtZpl::FieldKind::Text;
      });
      QVERIFY(text != analysis->fields.end());
      QVERIFY(text->hasUnknownCommands);
      QVERIFY(document->sourceText(text->sourceSpan).contains(u"^ZZfuture"));
    }
  }

  void analysisTracksTypesetContinuation() {
    const QString prefix = u"^XA^PW120^LL80^CFA,9,5"_s;
    const auto document = QtZpl::parse(prefix
      + u"^FT20,30^FDA^FS^FT,^FDB^FS^FT,50^FDC^FS^FT80,^FDD^FS^XZ");
    const auto explicitDocument = QtZpl::parse(prefix
      + u"^FT20,30^FDA^FS^FT25,30^FDB^FS^FT30,50^FDC^FS^FT80,50^FDD^FS^XZ");
    QVERIFY(document.has_value());
    QVERIFY(explicitDocument.has_value());
    const auto analysis = QtZpl::analyze(*document);
    const auto rendered = QtZpl::renderLabel(*document, 0, {.collectFieldGeometry = true});
    const auto control = QtZpl::renderLabel(*explicitDocument, 0);
    QVERIFY(analysis.has_value());
    QVERIFY(rendered.has_value());
    QVERIFY(control.has_value());
    QCOMPARE(rendered->image, control->image);
    const QList<QPoint> expected{QPoint(20, 30), QPoint(25, 30), QPoint(30, 50), QPoint(80, 50)};
    QCOMPARE(analysis->fields.size(), expected.size());
    QCOMPARE(rendered->fields.size(), expected.size());
    for (qsizetype i = 0; i < expected.size(); ++i) {
      QCOMPARE(analysis->fields[i].settings.position, expected[i]);
      QCOMPARE(rendered->fields[i].field.settings.position, expected[i]);
      QVERIFY(analysis->fields[i].settings.baseline);
      compareFieldIdentity(rendered->fields[i].field, analysis->fields[i]);
    }
  }

  void analysisTracksQrBarcodeDefaultSideEffect() {
    const QString prefix = u"^XA^PW400^LL260^BY1,3,30"_s;
    const QString qr = u"^FO10,10^BQN,2,4^FDMA,A^FS"_s;
    const QString subsequent = u"^FO150,10^B7N,3,2^FDabc^FS"_s;
    const QString reset = u"^BY2,3,30^FO10,170^BCN,30,N,N,N,A^FD1234^FS^XZ"_s;
    const auto document = QtZpl::parse(prefix + qr + subsequent + reset);
    QVERIFY(document.has_value());
    const auto analysis = QtZpl::analyze(*document);
    const auto rendered = QtZpl::renderLabel(*document, 0, {.collectFieldGeometry = true});
    const auto control = QtZpl::render(prefix + qr + u"^BY4,3,30" + subsequent + reset);
    QVERIFY(analysis.has_value());
    QVERIFY(rendered.has_value());
    QVERIFY(control.has_value());
    QVERIFY(rendered->diagnostics.isEmpty());
    QCOMPARE(rendered->image, control->labels.front());
    QCOMPARE(analysis->fields.size(), 3);
    QCOMPARE(rendered->fields.size(), 3);
    for (qsizetype i = 0; i < 3; ++i) {
      const int width = i == 2 ? 2 : 4;
      QCOMPARE(analysis->fields[i].settings.barcodeDefaults.moduleWidth, width);
      QCOMPARE(rendered->fields[i].field.settings.barcodeDefaults.moduleWidth, width);
      compareFieldIdentity(rendered->fields[i].field, analysis->fields[i]);
    }
  }

  void analysisRetainsAmbiguityWhenUnknownCommandsAreNotPreserved() {
    const auto document = QtZpl::parse(
      u"^XA^ZZfuture^PW80^LL60^FO1,2^FDone^FS"
      u"^ZZlater^FO4,5^FDtwo^FS^XZ", {.preserveUnknownCommands = false});
    QVERIFY(document.has_value());
    const auto analysis = QtZpl::analyze(*document);
    const auto rendered = QtZpl::renderLabel(*document, 0, {.collectFieldGeometry = true});
    QVERIFY(analysis.has_value());
    QVERIFY(rendered.has_value());
    QCOMPARE(analysis->fields.size(), 2);
    QCOMPARE(rendered->fields.size(), 2);
    for (qsizetype i = 0; i < analysis->fields.size(); ++i) {
      const auto& field = analysis->fields[i];
      QCOMPARE(field.kind, QtZpl::FieldKind::Text);
      QVERIFY(field.hasUnknownCommands);
      QVERIFY(rendered->fields[i].field.hasUnknownCommands);
      QVERIFY(document->sourceText(field.sourceSpan).contains(i == 0 ? u"^ZZfuture" : u"^ZZlater"));
      QVERIFY(document->sourceText(field.sourceSpan).endsWith(u"^FS"));
    }
  }

  void geometryRetainsWhiteReverseCoveredAndClippedFields() {
    const auto document = QtZpl::parse(
      u"^XA^PW80^LL60"
      u"^FO5,8^GB20,12,12^FS^FO5,8^GB20,12,12,W^FS"
      u"^FO35,8^FR^GB10,10,10^FS"
      u"^FO75,55^GB10,10,10^FS^FO90,70^GB10,10,10^FS"
      u"^FO0,0^FD^FS^XZ");
    QVERIFY(document.has_value());
    const auto plain = QtZpl::render(*document);
    const auto geometry = QtZpl::render(*document, {.collectFieldGeometry = true});
    const auto analysis = QtZpl::analyze(*document);
    QVERIFY(plain.has_value());
    QVERIFY(geometry.has_value());
    QVERIFY(analysis.has_value());
    QCOMPARE(geometry->labels, plain->labels);
    QVERIFY(plain->fields.isEmpty());
    QCOMPARE(geometry->fields.size(), 6);
    QCOMPARE(analysis->fields.size(), 6);
    const QList<QRect> expected{
      QRect(5, 8, 20, 12), QRect(5, 8, 20, 12), QRect(35, 8, 10, 10),
      QRect(75, 55, 10, 10), QRect(90, 70, 10, 10)
    };
    for (qsizetype i = 0; i < expected.size(); ++i) {
      const auto& field = geometry->fields[i];
      compareFieldIdentity(field.field, analysis->fields[i]);
      QCOMPARE(field.logicalBounds, expected[i]);
      QCOMPARE(field.paintBounds, expected[i]);
      QCOMPARE(field.clippedBounds, expected[i].intersected(QRect(0, 0, 80, 60)));
      QCOMPARE(field.status, i == 4 ? QtZpl::FieldStatus::Clipped : QtZpl::FieldStatus::Drawn);
    }
    QVERIFY(geometry->fields[2].field.settings.reverse);
    QCOMPARE(geometry->fields.back().status, QtZpl::FieldStatus::Empty);
    // The second field erased the first one's ink, but both own bounds survive.
    QCOMPARE(geometry->labels.front().pixelColor(10, 10), QColor(Qt::white));
    QCOMPARE(geometry->labels.front().pixelColor(38, 10), QColor(Qt::black));
  }

  void verticalFontZeroDefaultWidthHasLogicalGeometry() {
    const auto document = QtZpl::parse(
      u"^XA^PW100^LL200^FO10,10^A0N,30,0^FPV^FDAB^FS^XZ");
    QVERIFY(document.has_value());
    const auto rendered = QtZpl::renderLabel(*document, 0, {.collectFieldGeometry = true});
    QVERIFY(rendered.has_value());
    QCOMPARE(rendered->fields.size(), 1);
    QCOMPARE(rendered->fields.front().logicalBounds, QRect(QPoint(10, 10), QSize(30, 60)));
    QVERIFY(!rendered->fields.front().paintBounds.isEmpty());
  }

  void zeroWidthFieldBlockKeepsPlainTextLogicalGeometry() {
    const auto plain = QtZpl::render(
      u"^XA^PW150^LL100^FO10,10^A0N,30,30^FDAB^FS^XZ", {},
      {.collectFieldGeometry = true});
    QVERIFY(plain.has_value());
    QCOMPARE(plain->fields.size(), 1);
    QVERIFY(!plain->fields.front().logicalBounds.isEmpty());
    for (const auto block : {QStringView{u"^FB0"}, QStringView{u"^FB"}}) {
      const auto rendered = QtZpl::render(
        u"^XA^PW150^LL100^FO10,10^A0N,30,30"_s + block + u"^FDAB^FS^XZ", {},
        {.collectFieldGeometry = true});
      QVERIFY(rendered.has_value());
      QCOMPARE(rendered->fields.size(), 1);
      QCOMPARE(rendered->fields.front().logicalBounds, plain->fields.front().logicalBounds);
      QCOMPARE(rendered->fields.front().paintBounds, plain->fields.front().paintBounds);
      QCOMPARE(rendered->labels, plain->labels);
    }
  }

  void encodedGraphicBudgetIsCheckedBeforeDecoding() {
    const auto graphic = graphicFrom(u"^XA^GFA,2,2,1,AA55^FS^XZ");
    QtZpl::GraphicDecodeOptions options;
    options.maxEncodedBytes = 3;
    const auto limited = QtZpl::decodeGraphic(graphic, options);
    QVERIFY(!limited.has_value());
    QCOMPARE(limited.error().code, u"graphic-input-limit"_s);
    options.maxEncodedBytes = 4;
    QVERIFY(QtZpl::decodeGraphic(graphic, options).has_value());
    options.maxEncodedBytes = -1;
    const auto invalid = QtZpl::decodeGraphic(graphic, options);
    QVERIFY(!invalid.has_value());
    QCOMPARE(invalid.error().code, u"invalid-graphic-options"_s);
  }

  void transparentInkKeepsLogicalGeometryWithoutPaint() {
    const auto document = QtZpl::parse(
      u"^XA^PW80^LL60^FO5,6^GB10,8,8^FS^FO20,6^GC10,3^FS"
      u"^FO35,6^GE12,10,2^FS^FO50,6^GD10,8,2^FS"
      u"^FO5,30^GFA,2,2,1,AA55^FS^XZ");
    QVERIFY(document.has_value());
    QtZpl::RenderOptions options;
    options.foreground = Qt::transparent;
    options.collectFieldGeometry = true;
    const auto rendered = QtZpl::renderLabel(*document, 0, options);
    QVERIFY(rendered.has_value());
    QCOMPARE(rendered->fields.size(), 5);
    for (const auto& field : rendered->fields) {
      QVERIFY(!field.logicalBounds.isEmpty());
      QVERIFY(field.paintBounds.isEmpty());
      QVERIFY(field.clippedBounds.isEmpty());
      QCOMPARE(field.status, QtZpl::FieldStatus::Empty);
    }
    QVERIFY(inkBounds(rendered->image).isEmpty());
    const auto white = QtZpl::parse(u"^XA^PW30^LL30^FO5,6^GB10,8,8,W^FS^XZ");
    QVERIFY(white.has_value());
    options.foreground = Qt::black;
    options.background = Qt::transparent;
    const auto transparentPaper = QtZpl::renderLabel(*white, 0, options);
    QVERIFY(transparentPaper.has_value());
    QCOMPARE(transparentPaper->fields.front().logicalBounds, QRect(5, 6, 10, 8));
    QVERIFY(transparentPaper->fields.front().paintBounds.isEmpty());
    QCOMPARE(transparentPaper->fields.front().status, QtZpl::FieldStatus::Empty);
    QCOMPARE(transparentPaper->image.pixelColor(8, 8).alpha(), 0);
  }

  void geometryFollowsLabelTransform_data() {
    QTest::addColumn<QChar>("orientation");
    QTest::addColumn<bool>("mirror");
    QTest::addColumn<QRect>("bounds");
    const QList<QRect> boxes{QRect(5, 8, 20, 12), QRect(40, 5, 12, 20),
                             QRect(55, 40, 20, 12), QRect(8, 55, 12, 20)};
    const QString orientations = u"NRIB"_s;
    for (qsizetype i = 0; i < orientations.size(); ++i)
      for (const bool mirror : {false, true}) {
        QRect expected = boxes[i];
        const int width = (i == 1 || i == 3) ? 60 : 80;
        if (mirror) expected.moveLeft(width - expected.x() - expected.width());
        const QByteArray name = QString(orientations[i]).toLatin1() + (mirror ? "-mirror" : "-normal");
        QTest::newRow(name.constData()) << orientations[i] << mirror << expected;
      }
  }

  void geometryFollowsLabelTransform() {
    QFETCH(QChar, orientation);
    QFETCH(bool, mirror);
    QFETCH(QRect, bounds);
    const auto document = QtZpl::parse(
      u"^XA^PW80^LL60^PO%1^PM%2^FO5,8^GB20,12,12^FS^XZ"_s
        .arg(orientation).arg(mirror ? u'Y' : u'N'));
    QVERIFY(document.has_value());
    const auto rendered = QtZpl::renderLabel(*document, 0, {.collectFieldGeometry = true});
    QVERIFY(rendered.has_value());
    QCOMPARE(rendered->fields.size(), 1);
    const auto& field = rendered->fields.front();
    QCOMPARE(field.logicalBounds, bounds);
    QCOMPARE(field.paintBounds, bounds);
    QCOMPARE(field.clippedBounds, bounds);
    QCOMPARE(inkBounds(rendered->image), bounds);
    QCOMPARE(field.anchor, field.labelTransform.map(QPoint(5, 8)));
  }

  void textAndBarcodeGeometryEnclosesOwnPaint_data() {
    QTest::addColumn<QString>("field");
    for (const QChar orientation : QStringView{u"NRIB"}) {
      for (const bool baseline : {false, true}) {
        const auto name = QString(orientation).toLatin1() + (baseline ? "-FT" : "-FO");
        const auto origin = baseline ? u"^FT180,180"_s : u"^FO180,180"_s;
        QTest::newRow((name + "-text").constData())
          << origin + u"^A0%1,25,18^FB100,2,3,C^FDAgjp\\&two^FS"_s.arg(orientation);
        QTest::newRow((name + "-barcode").constData())
          << origin + u"^BY2,2,40^BC%1,40,Y,N,N,A^FD123456^FS"_s.arg(orientation);
      }
    }
  }

  void textAndBarcodeGeometryEnclosesOwnPaint() {
    QFETCH(QString, field);
    const auto document = QtZpl::parse(u"^XA^PW500^LL500"_s + field + u"^XZ");
    QVERIFY(document.has_value());
    const auto plain = QtZpl::renderLabel(*document, 0);
    const auto rendered = QtZpl::renderLabel(*document, 0, {.collectFieldGeometry = true});
    QVERIFY(plain.has_value());
    QVERIFY(rendered.has_value());
    QCOMPARE(plain->image, rendered->image);
    QCOMPARE(rendered->fields.size(), 1);
    const auto& geometry = rendered->fields.front();
    const auto painted = inkBounds(rendered->image);
    QVERIFY(!painted.isEmpty());
    QVERIFY(!geometry.logicalBounds.isEmpty());
    if (geometry.field.kind == QtZpl::FieldKind::Text) QCOMPARE(geometry.paintBounds, painted);
    QVERIFY2(geometry.paintBounds.contains(painted), qPrintable(
      u"Paint bounds %1,%2 %3x%4 must enclose ink %5,%6 %7x%8"_s
        .arg(geometry.paintBounds.x()).arg(geometry.paintBounds.y())
        .arg(geometry.paintBounds.width()).arg(geometry.paintBounds.height())
        .arg(painted.x()).arg(painted.y()).arg(painted.width()).arg(painted.height())));
    QCOMPARE(geometry.clippedBounds, geometry.paintBounds.intersected(rendered->image.rect()));
    QCOMPARE(geometry.baseline, field.startsWith(u"^FT"));
    QCOMPARE(geometry.status, QtZpl::FieldStatus::Drawn);
  }

  void graphicGeometryIncludesWhiteInkClippingAndMirroring() {
    const QString command = u"^GFA,8,8,2,0000180042000000^FS^XZ"_s;
    for (const QPoint origin : {QPoint(10, 8), QPoint(-3, -1)})
      for (const bool white : {false, true})
        for (const bool mirror : {false, true}) {
          const auto document = QtZpl::parse(u"^XA^PW40^LL30^PM%1^FO%2,%3"_s
            .arg(mirror ? u'Y' : u'N').arg(origin.x()).arg(origin.y()) + command);
          QVERIFY(document.has_value());
          const QtZpl::RenderOptions options{.foreground = white ? Qt::white : Qt::black,
                                             .collectFieldGeometry = true};
          const auto rendered = QtZpl::renderLabel(*document, 0, options);
          QVERIFY(rendered.has_value());
          QCOMPARE(rendered->fields.size(), 1);
          QRect logical(origin, QSize(16, 4));
          QRect paint(origin + QPoint(1, 1), QSize(6, 2));
          if (mirror) {
            logical.moveLeft(40 - logical.x() - logical.width());
            paint.moveLeft(40 - paint.x() - paint.width());
          }
          const auto& field = rendered->fields.front();
          QCOMPARE(field.field.kind, QtZpl::FieldKind::Graphic);
          QCOMPARE(field.logicalBounds, logical);
          QCOMPARE(field.paintBounds, paint);
          QCOMPARE(field.clippedBounds, paint.intersected(rendered->image.rect()));
          QCOMPARE(field.status, QtZpl::FieldStatus::Drawn);
          if (white) QVERIFY(inkBounds(rendered->image).isEmpty());
          else QCOMPARE(inkBounds(rendered->image), field.clippedBounds);
        }
  }

  void binaryGraphicTypesetUsesPaddedRowHeight() {
    const auto bytes = QString::fromLatin1(QByteArray::fromHex("800080"));
    for (const bool right : {false, true}) {
      const auto document = QtZpl::parse(u"^XA^PW24^LL20^FT%1,10,%2^GFB,3,3,2,"_s
        .arg(right ? 20 : 10).arg(right ? 1 : 0) + bytes + u"^FS^XZ");
      QVERIFY(document.has_value());
      const auto rendered = QtZpl::renderLabel(*document, 0, {.collectFieldGeometry = true});
      QVERIFY(rendered.has_value());
      QVERIFY(rendered->diagnostics.isEmpty());
      QCOMPARE(rendered->fields.size(), 1);
      const int x = right ? 4 : 10;
      QCOMPARE(rendered->fields.front().logicalBounds, QRect(x, 8, 16, 2));
      QCOMPARE(rendered->fields.front().paintBounds, QRect(x, 8, 1, 2));
      QCOMPARE(inkBounds(rendered->image), QRect(x, 8, 1, 2));
    }
    const auto huge = QtZpl::parse(u"^XA^PW24^LL20^FT10,10,1^GFB,1,1,2147483647,X^FS^XZ");
    QVERIFY(huge.has_value());
    // A permissive caller budget must not bypass the decoder's checked stride.
    const auto rendered = QtZpl::renderLabel(*huge, 0,
      {.collectFieldGeometry = true, .maxGraphicBytes = 2147483647});
    QVERIFY(rendered.has_value());
    QVERIFY(!rendered->diagnostics.isEmpty());
    QCOMPARE(rendered->fields.size(), 1);
    QCOMPARE(rendered->fields.front().status, QtZpl::FieldStatus::Error);
    QVERIFY(rendered->fields.front().paintBounds.isEmpty());
    QVERIFY(inkBounds(rendered->image).isEmpty());
  }

  void primitivePaintBoundsMatchOwnRaster_data() {
    QTest::addColumn<QString>("command");
    for (const auto& command : {u"^GC30,2"_s, u"^GC30,30"_s,
                                u"^GE50,20,2"_s, u"^GE50,20,20"_s,
                                u"^GD40,25,3,B,L"_s, u"^GD40,25,3,B,R"_s})
      QTest::newRow(qPrintable(command)) << command;
  }

  void primitivePaintBoundsMatchOwnRaster() {
    QFETCH(QString, command);
    const auto document = QtZpl::parse(u"^XA^PW100^LL100^FO20,20"_s + command + u"^FS^XZ");
    QVERIFY(document.has_value());
    const auto original = QtZpl::renderLabel(*document, 0);
    const auto rendered = QtZpl::renderLabel(*document, 0, {.collectFieldGeometry = true});
    QVERIFY(original.has_value());
    QVERIFY(rendered.has_value());
    QCOMPARE(rendered->image, original->image);
    QCOMPARE(rendered->fields.size(), 1);
    const auto& field = rendered->fields.front();
    QCOMPARE(field.paintBounds, inkBounds(original->image));
    QCOMPARE(field.clippedBounds, field.paintBounds);
    QCOMPARE(field.status, QtZpl::FieldStatus::Drawn);
  }

  void renderBudgetsCancellationAndInvalidSelection() {
    const auto document = QtZpl::parse(u"^XA^PW8^LL8^XZ^XA^PW8^LL8^XZ");
    QVERIFY(document.has_value());
    for (const int index : {-1, 2}) {
      const auto rendered = QtZpl::renderLabel(*document, index);
      const auto analyzed = QtZpl::analyzeLabel(*document, index);
      QVERIFY(!rendered.has_value());
      QVERIFY(!analyzed.has_value());
      QCOMPARE(rendered.error().code, u"invalid-label-index"_s);
      QCOMPARE(analyzed.error().code, u"invalid-label-index"_s);
    }
    const auto overBudget = QtZpl::render(*document, {.maxTotalPixels = 100});
    QVERIFY(!overBudget.has_value());
    QCOMPARE(overBudget.error().code, u"render-pixel-limit"_s);
    QVERIFY(QtZpl::renderLabel(*document, 1, {.maxTotalPixels = 100}).has_value());
    QVERIFY(QtZpl::analyze(*document, {.maxTotalPixels = 1}).has_value());
    const auto invalidOptions = QtZpl::render(*document, {.maxTotalPixels = -1});
    QVERIFY(!invalidOptions.has_value());
    QCOMPARE(invalidOptions.error().code, u"invalid-render-options"_s);
    const auto invalidDpi = QtZpl::renderLabel(*document, 0, {.dpi = 72});
    QVERIFY(!invalidDpi.has_value());
    QCOMPARE(invalidDpi.error().code, u"invalid-dpi"_s);
    std::stop_source stop;
    stop.request_stop();
    QtZpl::RenderOptions cancelled;
    cancelled.stopToken = stop.get_token();
    const auto renderCancelled = QtZpl::render(*document, cancelled);
    const auto analysisCancelled = QtZpl::analyze(*document, cancelled);
    QVERIFY(!renderCancelled.has_value());
    QVERIFY(!analysisCancelled.has_value());
    QCOMPARE(renderCancelled.error().code, u"operation-cancelled"_s);
    QCOMPARE(analysisCancelled.error().code, u"operation-cancelled"_s);
    const auto warnings = QtZpl::parse(u"^XA^ZZone^ZZtwo^XZ");
    QVERIFY(warnings.has_value());
    const auto diagnosticLimit = QtZpl::render(*warnings, {.maxDiagnostics = 1});
    QVERIFY(!diagnosticLimit.has_value());
    QCOMPARE(diagnosticLimit.error().code, u"render-diagnostic-limit"_s);
  }

  void decodeGraphicMatchesPackedBytesAndRenderer_data() {
    QTest::addColumn<QString>("command");
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<int>("bytesPerRow");
    QTest::newRow("ascii") << u"^GFA,2,2,1,AA55"_s << QByteArray::fromHex("aa55") << 1;
    QTest::newRow("compressed") << u"^GFA,8,8,2,HA,:!,"_s
      << QByteArray::fromHex("aa00aa00ffff0000") << 2;
    QTest::newRow("z64") << u"^GFA,10,2,1,:Z64:eJxbFQoAAasBAA==:B023"_s
      << QByteArray::fromHex("aa55") << 1;
    QTest::newRow("binary-partial-row")
      << u"^GFB,7,7,2,"_s + QString::fromLatin1(QByteArray::fromHex("81ff00245a5e7e"))
      << QByteArray::fromHex("81ff00245a5e7e00") << 2;
  }

  void decodeGraphicMatchesPackedBytesAndRenderer() {
    QFETCH(QString, command);
    QFETCH(QByteArray, bytes);
    QFETCH(int, bytesPerRow);
    const auto graphic = graphicFrom(u"^XA"_s + command + u"^FS^XZ");
    const auto decoded = QtZpl::decodeGraphic(graphic);
    QVERIFY(decoded.has_value());
    QCOMPARE(decoded->bytes, bytes);
    QCOMPARE(decoded->bytesPerRow, bytesPerRow);
    const QSize size(bytesPerRow * 8, int(bytes.size() / bytesPerRow));
    QCOMPARE(decoded->size, size);
    const auto rendered = QtZpl::render(u"^XA^PW%1^LL%2"_s.arg(size.width()).arg(size.height())
      + command + u"^FS^XZ");
    QVERIFY(rendered.has_value());
    QVERIFY(rendered->diagnostics.isEmpty());
    for (int y = 0; y < size.height(); ++y)
      for (int x = 0; x < size.width(); ++x) {
        const auto byte = static_cast<unsigned char>(bytes[y * bytesPerRow + x / 8]);
        const QColor expected = byte & (0x80U >> (x % 8)) ? Qt::black : Qt::white;
        QCOMPARE(rendered->labels.front().pixelColor(x, y), expected);
      }
  }

  void decodeGraphicErrorsMatchRenderer_data() {
    QTest::addColumn<QString>("command");
    QTest::addColumn<QString>("code");
    QTest::newRow("crc") << u"^GFA,10,2,1,:Z64:eJxbFQoAAasBAA==:B022"_s << u"graphic-field-z64-crc"_s;
    QTest::newRow("base64") << u"^GFA,4,2,1,:Z64:!!!!:F198"_s << u"graphic-field-z64-base64"_s;
    QTest::newRow("zlib") << u"^GFA,3,2,1,:Z64:AAAA:54AD"_s << u"graphic-field-z64-zlib"_s;
    QTest::newRow("forged-z64-decompressed-size")
      << u"^GFA,10,1,1,:Z64:eJxbFQoAAasBAA==:B023"_s << u"graphic-field-z64-zlib"_s;
    QTest::newRow("incomplete-ascii") << u"^GFA,1,1,1,F"_s << u"graphic-field-ascii-compression"_s;
    QTest::newRow("missing-repeat-row") << u"^GFA,1,1,1,:"_s << u"graphic-field-ascii-compression"_s;
  }

  void decodeGraphicErrorsMatchRenderer() {
    QFETCH(QString, command);
    QFETCH(QString, code);
    const auto source = u"^XA^PW8^LL2"_s + command + u"^FS^XZ";
    const auto decoded = QtZpl::decodeGraphic(graphicFrom(source));
    QVERIFY(!decoded.has_value());
    QCOMPARE(decoded.error().code, code);
    const auto rendered = QtZpl::render(source);
    QVERIFY(rendered.has_value());
    QVERIFY(std::ranges::any_of(rendered->diagnostics, [&](const auto& diagnostic) {
      return diagnostic.severity == QtZpl::Severity::Error && diagnostic.code == code;
    }));
    QVERIFY(inkBounds(rendered->labels.front()).isEmpty());
  }

  void graphicBudgetsCancellationAndPublicInputValidation() {
    const auto graphic = graphicFrom(u"^XA^GFA,2,2,1,AA55^FS^XZ");
    const auto tooSmall = QtZpl::decodeGraphic(graphic, {.maxDecodedBytes = 1});
    QVERIFY(!tooSmall.has_value());
    QCOMPARE(tooSmall.error().code, u"graphic-byte-limit"_s);
    QVERIFY(QtZpl::decodeGraphic(graphic, {.maxDecodedBytes = 2}).has_value());
    const auto invalidOptions = QtZpl::decodeGraphic(graphic, {.maxDecodedBytes = -1});
    QVERIFY(!invalidOptions.has_value());
    QCOMPARE(invalidOptions.error().code, u"invalid-graphic-options"_s);
    // Limits include zero padding of a partial binary row, before allocation.
    const QtZpl::GraphicField partial{u'B', 3, 3, 2, QByteArray::fromHex("aa55ff")};
    const auto paddingLimit = QtZpl::decodeGraphic(partial, {.maxDecodedBytes = 3});
    QVERIFY(!paddingLimit.has_value());
    QCOMPARE(paddingLimit.error().code, u"graphic-byte-limit"_s);
    const auto padded = QtZpl::decodeGraphic(partial, {.maxDecodedBytes = 4});
    QVERIFY(padded.has_value());
    QCOMPARE(padded->bytes, QByteArray::fromHex("aa55ff00"));
    const auto wrongBinaryLength = QtZpl::decodeGraphic({u'B', 3, 3, 2, QByteArray::fromHex("aa55")});
    QVERIFY(!wrongBinaryLength.has_value());
    QCOMPARE(wrongBinaryLength.error().code, u"graphic-field-binary-size"_s);
    const auto unsupported = QtZpl::decodeGraphic({u'C', 2, 2, 1, QByteArray("data")});
    QVERIFY(!unsupported.has_value());
    QCOMPARE(unsupported.error().code, u"graphic-field-compression"_s);
    const auto invalidStride = QtZpl::decodeGraphic({u'B', 2, 2, 0, QByteArray::fromHex("aa55")});
    QVERIFY(!invalidStride.has_value());
    const auto huge = QtZpl::decodeGraphic({u'A', 2147483647, 2147483647, 1, {}});
    QVERIFY(!huge.has_value());
    QCOMPARE(huge.error().code, u"graphic-byte-limit"_s);
    std::stop_source stop;
    stop.request_stop();
    const auto cancelled = QtZpl::decodeGraphic(graphic, {.stopToken = stop.get_token()});
    QVERIFY(!cancelled.has_value());
    QCOMPARE(cancelled.error().code, u"operation-cancelled"_s);

    const auto document = QtZpl::parse(u"^XA^PW8^LL2^GFA,2,2,1,AA55^FS^XZ");
    QVERIFY(document.has_value());
    const auto renderLimit = QtZpl::render(*document, {.maxGraphicBytes = 1});
    QVERIFY(!renderLimit.has_value());
    QCOMPARE(renderLimit.error().code, u"render-graphic-limit"_s);
  }
};

QTEST_MAIN(DesignerApiTest)
#include "test_designer_api.moc"
