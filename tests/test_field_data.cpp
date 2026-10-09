#include <QtTest/QTest>
#include <QtZpl/qtzpl.hpp>
#include "../src/barcode_encoders.hpp"

namespace {
using namespace Qt::StringLiterals;

QList<const QtZpl::Command*> fieldsOf(const QtZpl::Document& document) {
  QList<const QtZpl::Command*> fields;
  for (const auto& label : document.labels())
    for (const auto& command : label.commands())
      if (std::holds_alternative<QtZpl::FieldData>(command.payload)) fields.append(&command);
  return fields;
}

QString hexField(const QByteArray& bytes) {
  QString text;
  for (const unsigned char byte : bytes)
    text += u'_' + QString::number(byte, 16).rightJustified(2, u'0');
  return text;
}

QString label(QStringView charset, QStringView barcode, QStringView field) {
  return u"^XA^PW640^LL480"_s + charset + u"^FO20,20^BY2,3,0" + barcode + field + u"^FS^XZ";
}

bool hasInk(const QImage& image) {
  for (int y = 0; y < image.height(); ++y)
    for (int x = 0; x < image.width(); ++x)
      if (image.pixelColor(x, y) != QColor(Qt::white)) return true;
  return false;
}

QString diagnosticsText(const QList<QtZpl::Diagnostic>& diagnostics) {
  QStringList messages;
  for (const auto& diagnostic : diagnostics)
    messages.append(diagnostic.code + u": " + diagnostic.message);
  return messages.join(u'\n');
}

struct BinaryBarcode {
  const char* name;
  QString command;
  QString prefix;
  QString encodingDiagnostic;
};

const QList<BinaryBarcode> binaryBarcodes{
  {"datamatrix", u"^BXN,2,200"_s, {}, u"datamatrix-encoding"_s},
  {"pdf417", u"^B7N,3,2"_s, {}, u"pdf417-encoding"_s},
  {"micropdf417", u"^BFN,3,33"_s, {}, u"barcode-encoding"_s},
  {"aztec-BO", u"^BON,2,N,23,N"_s, {}, u"barcode-encoding"_s},
  {"aztec-B0", u"^B0N,2,N,23,N"_s, {}, u"barcode-encoding"_s},
  {"maxicode", u"^BD4"_s, {}, u"maxicode-encode"_s},
  {"qr", u"^BQN,2,2"_s, u"MA,"_s, u"qrcode-encoding"_s}
};
}

class FieldDataTest final : public QObject {
  Q_OBJECT

private slots:
  void parsedTextAndBytes_data() {
    QTest::addColumn<QString>("input");
    QTest::addColumn<QString>("text");
    QTest::addColumn<QByteArray>("bytes");
    QTest::addColumn<bool>("available");
    QTest::addColumn<bool>("invalidUtf8");
    QTest::newRow("empty") << u"^XA^FD^FS^XZ"_s << QString{} << QByteArray{} << true << false;
    QTest::newRow("ascii") << u"^XA^FDa,b^FS^XZ"_s << u"a,b"_s << QByteArray("a,b") << true << false;
    QTest::newRow("latin1-default") << u"^XA^FDéÿ^FS^XZ"_s << u"éÿ"_s << QByteArray::fromHex("e9ff") << true << false;
    QTest::newRow("latin1-other-ci") << u"^XA^CI27^FDé^FS^XZ"_s << u"é"_s << QByteArray::fromHex("e9") << true << false;
    QTest::newRow("utf8-latin") << u"^XA^CI28^FDé^FS^XZ"_s << u"é"_s << QByteArray::fromHex("c3a9") << true << false;
    QTest::newRow("utf8-cyrillic") << u"^XA^CI28^FDЖя^FS^XZ"_s << u"Жя"_s << QByteArray::fromHex("d096d18f") << true << false;
    QTest::newRow("utf8-supplementary") << u"^XA^CI28^FD\U0001f600^FS^XZ"_s << u"\U0001f600"_s << QByteArray::fromHex("f09f9880") << true << false;
    QTest::newRow("utf8-escaped") << u"^XA^CI28^FH^FD_C3_A9_D0_96_F0_9F_98_80^FS^XZ"_s
      << u"éЖ\U0001f600"_s << QByteArray::fromHex("c3a9d096f09f9880") << true << false;
    QTest::newRow("utf8-mixed-fv") << u"^XA^CI28^FH!^FVé!D0!96\U0001f600!00^FS^XZ"_s
      << (u"éЖ\U0001f600"_s + QChar(0)) << QByteArray::fromHex("c3a9d096f09f988000") << true << false;
    QTest::newRow("binary-nul-ff") << u"^XA^FH^FD_00_ff_41^FS^XZ"_s
      << QString::fromLatin1(QByteArray::fromHex("00ff41")) << QByteArray::fromHex("00ff41") << true << false;
    QTest::newRow("utf8-invalid") << u"^XA^CI28^FH^FD_D0_FF^FS^XZ"_s
      << u"_D0_FF"_s << QByteArray::fromHex("d0ff") << true << true;
    QTest::newRow("utf8-invalid-mixed") << u"^XA^CI28^FH^FDé_00_FFЖ^FS^XZ"_s
      << u"é_00_FFЖ"_s << QByteArray::fromHex("c3a900ffd096") << true << true;
    QTest::newRow("utf8-invalid-run-before-tail") << u"^XA^CI28^FH^FD_D0_FF-tail_00^FS^XZ"_s
      << u"_D0_FF-tail_00"_s << QByteArray::fromHex("d0ff2d7461696c00") << true << true;
    QTest::newRow("invalid-hex-is-literal") << u"^XA^CI28^FH^FDé_GG_4^FS^XZ"_s
      << u"é_GG_4"_s << QByteArray::fromHex("c3a95f47475f34") << true << false;
    QTest::newRow("default-unicode-text") << u"^XA^FDЖ^FS^XZ"_s << u"Ж"_s << QByteArray{} << false << false;
    QTest::newRow("other-ci-unicode-text") << u"^XA^CI27^FH^FVЖ_41^FS^XZ"_s << u"ЖA"_s << QByteArray{} << false << false;
  }

  void parsedTextAndBytes() {
    QFETCH(QString, input);
    QFETCH(QString, text);
    QFETCH(QByteArray, bytes);
    QFETCH(bool, available);
    QFETCH(bool, invalidUtf8);
    const auto document = QtZpl::parse(input);
    QVERIFY(document.has_value());
    const auto fields = fieldsOf(*document);
    QCOMPARE(fields.size(), 1);
    const auto& field = std::get<QtZpl::FieldData>(fields.front()->payload);
    QCOMPARE(field.data, text);
    QCOMPARE(field.bytes.has_value(), available);
    if (available) QCOMPARE(*field.bytes, bytes);
    QCOMPARE(document->diagnostics().size(), invalidUtf8 ? 1 : 0);
    if (invalidUtf8) {
      const auto& diagnostic = document->diagnostics().front();
      QCOMPARE(diagnostic.severity, QtZpl::Severity::Warning);
      QCOMPARE(diagnostic.code, u"invalid-field-encoding"_s);
      QCOMPARE(diagnostic.offset, fields.front()->offset);
      QCOMPARE(diagnostic.command, fields.front()->source);
    }
  }

  void fieldAndCharsetScope() {
    const auto document = QtZpl::parse(
      u"^XA^CI28^FH^FD_C3_A9^FS^FD_C3_A9^FS"
      u"^FH!^FV!D0!96^FS^FV!D0!96^FS^FH^FS^FD_41^FS"
      u"^CI0^FH^FD_C3_A9^FS^CI28^FDé^FS^FH^XZ"
      u"^XA^FD_41^FS^FDé^FS^CI0^FDé^FS^XZ");
    QVERIFY(document.has_value());
    QVERIFY(document->diagnostics().isEmpty());
    QCOMPARE(document->labels().size(), 2);
    const auto fields = fieldsOf(*document);
    const QList<QByteArray> expectedBytes{
      QByteArray::fromHex("c3a9"), "_C3_A9", QByteArray::fromHex("d096"), "!D0!96", "_41",
      QByteArray::fromHex("c3a9"), QByteArray::fromHex("c3a9"), "_41", QByteArray::fromHex("c3a9"), QByteArray::fromHex("e9")
    };
    const QStringList expectedText{
      u"é"_s, u"_C3_A9"_s, u"Ж"_s, u"!D0!96"_s, u"_41"_s,
      QString::fromLatin1(QByteArray::fromHex("c3a9")), u"é"_s, u"_41"_s, u"é"_s, u"é"_s
    };
    QCOMPARE(fields.size(), expectedBytes.size());
    for (qsizetype i = 0; i < fields.size(); ++i) {
      const auto& field = std::get<QtZpl::FieldData>(fields[i]->payload);
      QVERIFY(field.bytes.has_value());
      QCOMPARE(*field.bytes, expectedBytes[i]);
      QCOMPARE(field.data, expectedText[i]);
    }
  }

  void binaryBarcodeUtf8RoundTrip_data() {
    QTest::addColumn<QString>("barcode");
    QTest::addColumn<QString>("prefix");
    QTest::addColumn<QString>("payload");
    for (const auto& barcode : binaryBarcodes) {
      for (const auto& payload : {u"é"_s, u"Ж"_s, u"éЖ\U0001f600"_s}) {
        const QByteArray name = QByteArray(barcode.name) + '-' + payload.toUtf8().toHex();
        QTest::newRow(name.constData()) << barcode.command << barcode.prefix << payload;
      }
    }
  }

  void binaryBarcodeUtf8RoundTrip() {
    QFETCH(QString, barcode);
    QFETCH(QString, prefix);
    QFETCH(QString, payload);
    const QString escaped = hexField(prefix.toLatin1() + payload.toUtf8());
    const auto expected = QtZpl::render(label(u"^CI0", barcode, u"^FH^FD" + escaped));
    QVERIFY(expected.has_value());
    QVERIFY2(expected->diagnostics.isEmpty(), qPrintable(diagnosticsText(expected->diagnostics)));
    QCOMPARE(expected->labels.size(), 1);
    QVERIFY(hasInk(expected->labels.front()));
    for (const QString& field : {u"^FD" + prefix + payload, u"^FH^FD" + escaped,
                                u"^FH^FV" + prefix + payload.left(1) + hexField(payload.mid(1).toUtf8())}) {
      const auto actual = QtZpl::render(label(u"^CI28", barcode, field));
      QVERIFY(actual.has_value());
      QVERIFY2(actual->diagnostics.isEmpty(), qPrintable(diagnosticsText(actual->diagnostics)));
      QCOMPARE(actual->labels.size(), 1);
      QCOMPARE(actual->labels.front(), expected->labels.front());
    }
  }

  void invalidUtf8BarcodeRetainsBytes_data() {
    QTest::addColumn<QString>("barcode");
    QTest::addColumn<QString>("prefix");
    for (const auto& barcode : binaryBarcodes)
      QTest::newRow(barcode.name) << barcode.command << barcode.prefix;
  }

  void invalidUtf8BarcodeRetainsBytes() {
    QFETCH(QString, barcode);
    QFETCH(QString, prefix);
    const QString field = u"^FH^FD" + hexField(prefix.toLatin1() + QByteArray::fromHex("4100ffd042"));
    const auto expected = QtZpl::render(label(u"^CI0", barcode, field));
    const auto actual = QtZpl::render(label(u"^CI28", barcode, field));
    QVERIFY(expected.has_value());
    QVERIFY(actual.has_value());
    QVERIFY2(expected->diagnostics.isEmpty(), qPrintable(diagnosticsText(expected->diagnostics)));
    QCOMPARE(actual->diagnostics.size(), 1);
    QCOMPARE(actual->diagnostics.front().code, u"invalid-field-encoding"_s);
    QCOMPARE(actual->diagnostics.front().severity, QtZpl::Severity::Warning);
    QCOMPARE(expected->labels.size(), 1);
    QCOMPARE(actual->labels.size(), 1);
    QVERIFY(hasInk(expected->labels.front()));
    QCOMPARE(actual->labels.front(), expected->labels.front());
  }

  void unsupportedUnicodeHasNoSymbol_data() {
    QTest::addColumn<QString>("barcode");
    QTest::addColumn<QString>("prefix");
    QTest::addColumn<QString>("diagnostic");
    for (const auto& barcode : binaryBarcodes)
      QTest::newRow(barcode.name) << barcode.command << barcode.prefix << barcode.encodingDiagnostic;
  }

  void unsupportedUnicodeHasNoSymbol() {
    QFETCH(QString, barcode);
    QFETCH(QString, prefix);
    QFETCH(QString, diagnostic);
    const auto document = QtZpl::parse(label(u"^CI0", barcode, u"^FD" + prefix + u"Ж"));
    QVERIFY(document.has_value());
    QVERIFY(document->diagnostics().isEmpty());
    const auto result = QtZpl::render(*document);
    QVERIFY(result.has_value());
    QCOMPARE(result->diagnostics.size(), 1);
    QCOMPARE(result->diagnostics.front().code, diagnostic);
    QCOMPARE(result->diagnostics.front().severity, QtZpl::Severity::Error);
    QCOMPARE(result->labels.size(), 1);
    QVERIFY(!hasInk(result->labels.front()));
  }

  void dataMatrixAndQrUseExactEncoderBytes_data() {
    QTest::addColumn<bool>("qr");
    QTest::addColumn<QByteArray>("bytes");
    for (const bool qr : {false, true})
      for (const auto& bytes : {QByteArray::fromHex("c3a9d096f09f9880"), QByteArray::fromHex("4100ff42")})
        QTest::newRow(((qr ? QByteArray("qr-") : QByteArray("dm-")) + bytes.toHex()).constData()) << qr << bytes;
  }

  void dataMatrixAndQrUseExactEncoderBytes() {
    QFETCH(bool, qr);
    QFETCH(QByteArray, bytes);
    const auto matrix = qr ? QtZpl::BarcodeEncoders::qrCode(bytes, u'M')
                           : QtZpl::BarcodeEncoders::dataMatrix(bytes, false);
    QVERIFY(matrix.has_value());
    const auto result = QtZpl::render(label(u"^CI0", qr ? u"^BQN,2,2" : u"^BXN,2,200",
      u"^FH^FD" + hexField((qr ? QByteArray("MA,") : QByteArray{}) + bytes)));
    QVERIFY(result.has_value());
    QVERIFY2(result->diagnostics.isEmpty(), qPrintable(diagnosticsText(result->diagnostics)));
    QImage expected(640, 480, QImage::Format_ARGB32_Premultiplied);
    expected.fill(Qt::white);
    // Independent dot expansion checks the encoder payload and complete image,
    // including absence of accidental text or extra modules outside the symbol.
    for (int y = 0; y < matrix->height * 2; ++y)
      for (int x = 0; x < matrix->width * 2; ++x)
        if (matrix->at(x / 2, y / 2)) expected.setPixelColor(20 + x, 20 + y, Qt::black);
    QCOMPARE(result->labels.size(), 1);
    QCOMPARE(result->labels.front(), expected);
  }

  void qrManualBinaryLengthCountsBytes_data() {
    QTest::addColumn<int>("length");
    for (const int length : {1, 2, 4, 8})
      QTest::newRow(QByteArray::number(length).constData()) << length;
  }

  void qrManualBinaryLengthCountsBytes() {
    QFETCH(int, length);
    const QString payload = u"éЖ\U0001f600trailing"_s;
    const auto expected = QtZpl::render(label(u"^CI0", u"^BQN,2,2",
      u"^FH^FD" + hexField(QByteArray("QA,") + payload.toUtf8().first(length))));
    const auto actual = QtZpl::render(label(u"^CI28", u"^BQN,2,2",
      u"^FDQM,B" + QString::number(length).rightJustified(4, u'0') + payload));
    QVERIFY(expected.has_value());
    QVERIFY(actual.has_value());
    QVERIFY2(expected->diagnostics.isEmpty(), qPrintable(diagnosticsText(expected->diagnostics)));
    QVERIFY2(actual->diagnostics.isEmpty(), qPrintable(diagnosticsText(actual->diagnostics)));
    QCOMPARE(expected->labels.size(), 1);
    QCOMPARE(actual->labels.size(), 1);
    QVERIFY(hasInk(expected->labels.front()));
    QCOMPARE(actual->labels.front(), expected->labels.front());
  }

  void gs1DataMatrixControlsFollowFieldDecoding() {
    const QString input = label(u"^CI28", u"^BXN,2,200,,,1,|",
      u"^FH^FD|101é|d02921_D0_96");
    const auto document = QtZpl::parse(input);
    QVERIFY(document.has_value());
    QVERIFY(document->diagnostics().isEmpty());
    const auto fields = fieldsOf(*document);
    QCOMPARE(fields.size(), 1);
    const auto& field = std::get<QtZpl::FieldData>(fields.front()->payload);
    QCOMPARE(field.data, u"|101é|d02921Ж"_s);
    QVERIFY(field.bytes.has_value());
    QCOMPARE(*field.bytes, QByteArray("|101") + QByteArray::fromHex("c3a9")
      + QByteArray("|d02921") + QByteArray::fromHex("d096"));

    const QByteArray payload = QByteArray("01") + QByteArray::fromHex("c3a91d")
      + QByteArray("21") + QByteArray::fromHex("d096");
    const auto matrix = QtZpl::BarcodeEncoders::dataMatrix(payload, true);
    QVERIFY(matrix.has_value());
    const auto result = QtZpl::render(*document);
    QVERIFY(result.has_value());
    QVERIFY2(result->diagnostics.isEmpty(), qPrintable(diagnosticsText(result->diagnostics)));
    QImage expected(640, 480, QImage::Format_ARGB32_Premultiplied);
    expected.fill(Qt::white);
    for (int y = 0; y < matrix->height * 2; ++y)
      for (int x = 0; x < matrix->width * 2; ++x)
        if (matrix->at(x / 2, y / 2)) expected.setPixelColor(20 + x, 20 + y, Qt::black);
    QCOMPARE(result->labels.size(), 1);
    QCOMPARE(result->labels.front(), expected);
  }
};

QTEST_MAIN(FieldDataTest)
#include "test_field_data.moc"
