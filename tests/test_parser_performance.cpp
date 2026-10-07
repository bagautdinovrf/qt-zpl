#include <QtTest/QtTest>
#include <QtZpl/qtzpl.hpp>

namespace {
using namespace Qt::StringLiterals;

template<class T>
QList<const QtZpl::Command*> commandsOf(const QtZpl::Document& document) {
  QList<const QtZpl::Command*> result;
  for (const auto& label : document.labels())
    for (const auto& command : label.commands())
      if (std::holds_alternative<T>(command.payload)) result.append(&command);
  return result;
}
}

class ParserPerformanceTest final : public QObject {
  Q_OBJECT

private slots:
  void rawFieldsPreserveContent_data() {
    QTest::addColumn<QString>("opcode");
    QTest::newRow("FD") << u"FD"_s;
    QTest::newRow("FV") << u"FV"_s;
  }

  void rawFieldsPreserveContent() {
    QFETCH(QString, opcode);
    const QString payload = QString(u"word,,"_s).repeated(4096)
      + u"Привет\r\n^FO123,456~ZZ,tail";
    const QString input = u"prefix^XA^" + opcode + payload + u"^FS^XZ";
    const auto parsed = QtZpl::parse(input);
    QVERIFY(parsed.has_value());
    QVERIFY(parsed->diagnostics().isEmpty());
    QCOMPARE(parsed->labels().size(), 1);
    const auto fields = commandsOf<QtZpl::FieldData>(*parsed);
    QCOMPARE(fields.size(), 1);
    QCOMPARE(std::get<QtZpl::FieldData>(fields.front()->payload).data, payload);
    QCOMPARE(fields.front()->source, u'^' + opcode + payload);
    QCOMPARE(fields.front()->offset, qsizetype(9));
  }

  void hexEscapesAndScope() {
    const auto parsed = QtZpl::parse(
      u"^XA^FH^FD_41_2c_aB_00_ff^FS^FD_41^FS"
      u"^FH!^FV!42_43!2C^FS^FV!42^FS^FH^XZ"
      u"^XA^FD_41^FS^XZ");
    QVERIFY(parsed.has_value());
    QVERIFY(parsed->diagnostics().isEmpty());
    const auto fields = commandsOf<QtZpl::FieldData>(*parsed);
    QCOMPARE(fields.size(), 5);
    QCOMPARE(std::get<QtZpl::FieldData>(fields[0]->payload).data,
             QString::fromLatin1(QByteArray::fromHex("412CAB00FF")));
    QCOMPARE(std::get<QtZpl::FieldData>(fields[1]->payload).data, u"_41"_s);
    QCOMPARE(std::get<QtZpl::FieldData>(fields[2]->payload).data, u"B_43,"_s);
    QCOMPARE(std::get<QtZpl::FieldData>(fields[3]->payload).data, u"!42"_s);
    QCOMPARE(std::get<QtZpl::FieldData>(fields[4]->payload).data, u"_41"_s);
    QCOMPARE(fields[0]->source, u"^FD_41_2c_aB_00_ff"_s);
  }

  void hexEscapesPreserveUnusualQtConversions() {
    const QStringList pairs{u"4G"_s, u"G4"_s, u" 4"_s, u"4 "_s,
      u"+F"_s, u"-1"_s, u"0x"_s, u"\tA"_s, u"\u00a0F"_s,
      u"ＦＦ"_s, u"aB"_s, u"00"_s, u"ff"_s};
    for (const auto& pair : pairs) {
      const QString input = u"^XA^FH^FD_" + pair + u"^FS^XZ";
      const auto parsed = QtZpl::parse(input);
      QVERIFY(parsed.has_value());
      const auto fields = commandsOf<QtZpl::FieldData>(*parsed);
      QCOMPARE(fields.size(), 1);
      bool ok = false;
      const auto byte = pair.toUInt(&ok, 16);
      const QString expected = ok ? QString(QChar::fromLatin1(static_cast<char>(byte)))
                                  : u'_' + pair;
      QCOMPARE(std::get<QtZpl::FieldData>(fields[0]->payload).data, expected);
    }
    const auto truncated = QtZpl::parse(u"^XA^FH^FDabc_4^FS^FH^FDabc_^FS^XZ");
    QVERIFY(truncated.has_value());
    const auto fields = commandsOf<QtZpl::FieldData>(*truncated);
    QCOMPARE(fields.size(), 2);
    QCOMPARE(std::get<QtZpl::FieldData>(fields[0]->payload).data, u"abc_4"_s);
    QCOMPARE(std::get<QtZpl::FieldData>(fields[1]->payload).data, u"abc_"_s);
  }

  void rawCommandsPreserveParametersAndSource() {
    const auto parsed = QtZpl::parse(
      u"^XA^FXcomment,,tail^F8one,,two^PR3,,5^FRignored,,^FSignored,,^XZ");
    QVERIFY(parsed.has_value());
    QVERIFY(parsed->diagnostics().isEmpty());
    const auto& commands = parsed->labels().front().commands();
    QCOMPARE(commands.size(), 7);
    QCOMPARE(std::get<QtZpl::Comment>(commands[1].payload).text, u"comment,,tail"_s);
    QCOMPARE(std::get<QtZpl::FieldEncoding>(commands[2].payload).parameters, u"one,,two"_s);
    QCOMPARE(std::get<QtZpl::PrintRate>(commands[3].payload).parameters, u"3,,5"_s);
    QVERIFY(std::holds_alternative<QtZpl::FieldReverse>(commands[4].payload));
    QVERIFY(std::holds_alternative<QtZpl::FieldSeparator>(commands[5].payload));
    QCOMPARE(commands[1].source, u"^FXcomment,,tail"_s);
    QCOMPARE(commands[4].source, u"^FRignored,,"_s);
    QCOMPARE(commands[5].source, u"^FSignored,,"_s);
  }

  void graphicHeadersPreserveCompressedAndBinaryData() {
    const auto ascii = QtZpl::parse(u"^XA^GFA,4,4,1,FF,,,^FS^XZ");
    QVERIFY(ascii.has_value());
    const auto graphics = commandsOf<QtZpl::GraphicField>(*ascii);
    QCOMPARE(graphics.size(), 1);
    const auto& graphic = std::get<QtZpl::GraphicField>(graphics[0]->payload);
    QCOMPARE(graphic.compression, u'A');
    QCOMPARE(graphic.totalBytes, 4);
    QCOMPARE(graphic.bytesUsed, 4);
    QCOMPARE(graphic.bytesPerRow, 1);
    QCOMPARE(graphic.data, QByteArray("FF,,,"));
    QCOMPARE(graphics[0]->source, u"^GFA,4,4,1,FF,,,"_s);

    const QByteArray binary = QByteArray::fromHex("2C5E7E00");
    const QString input = u"^XA^GFB,4,4,2," + QString::fromLatin1(binary)
      + u"^FS^FDafter^FS^XZ";
    const auto parsed = QtZpl::parse(input);
    QVERIFY(parsed.has_value());
    QVERIFY(parsed->diagnostics().isEmpty());
    const auto binaryGraphics = commandsOf<QtZpl::GraphicField>(*parsed);
    QCOMPARE(binaryGraphics.size(), 1);
    const auto& binaryGraphic = std::get<QtZpl::GraphicField>(binaryGraphics[0]->payload);
    QCOMPARE(binaryGraphic.compression, u'B');
    QCOMPARE(binaryGraphic.totalBytes, 4);
    QCOMPARE(binaryGraphic.bytesUsed, 4);
    QCOMPARE(binaryGraphic.bytesPerRow, 2);
    QCOMPARE(binaryGraphic.data, binary);
    const auto fields = commandsOf<QtZpl::FieldData>(*parsed);
    QCOMPARE(fields.size(), 1);
    QCOMPARE(std::get<QtZpl::FieldData>(fields[0]->payload).data, u"after"_s);
  }

  void graphicHeadersPreserveEmptyAndInvalidParameters_data() {
    QTest::addColumn<QString>("command");
    QTest::addColumn<int>("used");
    QTest::addColumn<int>("total");
    QTest::addColumn<int>("perRow");
    QTest::newRow("omitted") << u"^GFA"_s << 0 << 0 << 0;
    QTest::newRow("empty") << u"^GFA,,,,"_s << 0 << 0 << 0;
    QTest::newRow("missing-row") << u"^GFA,4,4"_s << 4 << 4 << 0;
    QTest::newRow("empty-row") << u"^GFA,4,4,,FF"_s << 4 << 4 << 0;
    QTest::newRow("invalid-counts") << u"^GFA,wrong,also-wrong,1,FF"_s << 0 << 0 << 1;
    QTest::newRow("empty-compression") << u"^GF,1,1,1,FF"_s << 1 << 1 << 1;
  }

  void graphicHeadersPreserveEmptyAndInvalidParameters() {
    QFETCH(QString, command);
    QFETCH(int, used);
    QFETCH(int, total);
    QFETCH(int, perRow);
    const auto parsed = QtZpl::parse(u"^XA" + command + u"^FS^XZ");
    QVERIFY(parsed.has_value());
    const auto graphics = commandsOf<QtZpl::GraphicField>(*parsed);
    QCOMPARE(graphics.size(), 1);
    const auto& graphic = std::get<QtZpl::GraphicField>(graphics[0]->payload);
    QCOMPARE(graphic.compression, u'A');
    QCOMPARE(graphic.bytesUsed, used);
    QCOMPARE(graphic.totalBytes, total);
    QCOMPARE(graphic.bytesPerRow, perRow);
    QCOMPARE(graphics[0]->source, command);
  }
};

QTEST_APPLESS_MAIN(ParserPerformanceTest)
#include "test_parser_performance.moc"
