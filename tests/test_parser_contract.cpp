#include <QtTest/QTest>
#include <QtZpl/qtzpl.hpp>

#include <future>
#include <limits>
#include <stop_token>
#include <vector>

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

class ParserContractTest final : public QObject {
  Q_OBJECT

private slots:
  void sourceOwnershipAndExactSpans() {
    const QString expected = u"before\r\n^xa\r\n^fo1,,2 \r\n^ci28^fh!^fdЖ!00\U0001f600\r\n^fs"
      u"^zz x,,y\r\n^fxcomment\r\n^fs^xz\r\nbetween ^xa^fdlast^fs^xz trailing"_s;
    auto input = expected;
    const auto document = QtZpl::parse(input);
    QVERIFY(document.has_value());
    input.fill(u'?');
    QCOMPARE(document->source(), expected);
    QCOMPARE(document->labels().size(), 2);
    const auto firstStart = expected.indexOf(u"^xa");
    const auto firstEnd = expected.indexOf(u"^xz") + 3;
    QCOMPARE(document->labels()[0].sourceSpan(), (QtZpl::SourceSpan{firstStart, firstEnd - firstStart}));
    QCOMPARE(document->sourceText(document->labels()[0].sourceSpan()),
      QStringView{expected}.sliced(firstStart, firstEnd - firstStart));
    const auto secondStart = expected.indexOf(u"^xa", firstEnd);
    const auto secondEnd = expected.indexOf(u"^xz", firstEnd) + 3;
    QCOMPARE(document->labels()[1].sourceSpan(), (QtZpl::SourceSpan{secondStart, secondEnd - secondStart}));
    for (const auto& label : document->labels()) {
      const auto labelSpan = label.sourceSpan();
      for (const auto& command : label.commands()) {
        QCOMPARE(command.offset, command.sourceSpan.start);
        QVERIFY(command.sourceSpan.start >= labelSpan.start);
        QVERIFY(command.sourceSpan.start + command.sourceSpan.length <= labelSpan.start + labelSpan.length);
        QCOMPARE(document->sourceText(command.sourceSpan),
          QStringView{expected}.sliced(command.sourceSpan.start, command.sourceSpan.length));
      }
    }
    const auto origins = commandsOf<QtZpl::FieldOrigin>(*document);
    QCOMPARE(origins.size(), 1);
    QCOMPARE(origins[0]->source, u"^FO1,,2 "_s);
    QCOMPARE(document->sourceText(origins[0]->sourceSpan), u"^fo1,,2 \r\n");
    const auto unknown = commandsOf<QtZpl::UnknownCommand>(*document);
    QCOMPARE(unknown.size(), 1);
    QCOMPARE(document->sourceText(unknown[0]->sourceSpan), u"^zz x,,y\r\n");
    const auto fields = commandsOf<QtZpl::FieldData>(*document);
    QCOMPARE(fields.size(), 2);
    const auto& field = std::get<QtZpl::FieldData>(fields[0]->payload);
    QCOMPARE(field.rawData, u"Ж!00\U0001f600\r\n"_s);
    QCOMPARE(document->sourceText(field.sourceSpan), field.rawData);
    QCOMPARE(field.characterSet, 28);
    QCOMPARE(field.hexIndicator, u'!');
    QVERIFY(field.bytes.has_value());
    QCOMPARE(*field.bytes, QByteArray::fromHex("d09600f09f98800d0a"));
    const auto& secondField = std::get<QtZpl::FieldData>(fields[1]->payload);
    QCOMPARE(secondField.characterSet, 28);
    QVERIFY(secondField.hexIndicator.isNull());
    QCOMPARE(document->diagnostics().size(), 1);
    const auto& diagnostic = document->diagnostics().front();
    QCOMPARE(diagnostic.labelIndex, std::optional<int>{0});
    QCOMPARE(diagnostic.sourceSpan, std::optional<QtZpl::SourceSpan>{unknown[0]->sourceSpan});
    QVERIFY(!diagnostic.fieldId.has_value());
  }

  void sourceViewChecksBounds() {
    const auto document = QtZpl::parse(u"^XA^XZ");
    QVERIFY(document.has_value());
    for (const auto span : {QtZpl::SourceSpan{}, QtZpl::SourceSpan{0, -1},
        QtZpl::SourceSpan{7, 0}, QtZpl::SourceSpan{5, 2},
        QtZpl::SourceSpan{1, std::numeric_limits<qsizetype>::max()}})
      QVERIFY(document->sourceText(span).isNull());
    const auto empty = document->sourceText({6, 0});
    QVERIFY(empty.isEmpty());
    QVERIFY(!empty.isNull());
  }

  void recoveryPreservesSource() {
    const QString input = u"prefix^XA^ZZignored^XA^FO2,3 trailing"_s;
    const auto document = QtZpl::parse(input, {.preserveUnknownCommands = false});
    QVERIFY(document.has_value());
    QCOMPARE(document->source(), input);
    QCOMPARE(document->labels().size(), 2);
    const auto secondStart = input.indexOf(u"^XA", 9);
    QCOMPARE(document->labels()[0].sourceSpan(), (QtZpl::SourceSpan{6, secondStart - 6}));
    QCOMPARE(document->labels()[1].sourceSpan(), (QtZpl::SourceSpan{secondStart, input.size() - secondStart}));
    QVERIFY(commandsOf<QtZpl::UnknownCommand>(*document).isEmpty());
    QCOMPARE(document->diagnostics().size(), 3);
    QCOMPARE(document->diagnostics()[0].code, u"unsupported-command"_s);
    QCOMPARE(document->diagnostics()[1].code, u"nested-format"_s);
    QCOMPARE(document->diagnostics()[2].code, u"unterminated-format"_s);
    QCOMPARE(document->diagnostics()[2].sourceSpan, (std::optional<QtZpl::SourceSpan>{{input.size(), 0}}));
    QCOMPARE(document->diagnostics()[2].labelIndex, std::optional<int>{1});
  }

  void syntaxChangesAndRestoration() {
    const QString input = u"~CC! !XA!CD;!FO12;34;1!BY3;;50!CT?!FH%!FD%D0%96!FS"
      u"!BXN;2;200;;;1;?!FD?10123?d029456!FS?CC/ /XZ"
      u"/XA/CD,/FO9,8/CC^ ^CT~^XZ"_s;
    const auto document = QtZpl::parse(input);
    QVERIFY(document.has_value());
    QVERIFY(document->diagnostics().isEmpty());
    QCOMPARE(document->labels().size(), 2);
    const auto origins = commandsOf<QtZpl::FieldOrigin>(*document);
    QCOMPARE(origins.size(), 2);
    const auto& first = std::get<QtZpl::FieldOrigin>(origins[0]->payload);
    QCOMPARE(first.x, 12);
    QCOMPARE(first.y, 34);
    QCOMPARE(first.justification, QtZpl::Justification::Right);
    QCOMPARE(std::get<QtZpl::FieldOrigin>(origins[1]->payload).x, 9);
    const auto defaults = commandsOf<QtZpl::BarcodeDefault>(*document);
    QCOMPARE(defaults.size(), 1);
    const auto& barcodeDefaults = std::get<QtZpl::BarcodeDefault>(defaults[0]->payload);
    QCOMPARE(barcodeDefaults.moduleWidth, 3);
    QCOMPARE(barcodeDefaults.wideToNarrowRatio, 3.0);
    QCOMPARE(barcodeDefaults.height, 50);
    const auto barcodes = commandsOf<QtZpl::Barcode>(*document);
    QCOMPARE(barcodes.size(), 1);
    QCOMPARE(std::get<QtZpl::Barcode>(barcodes[0]->payload).parameters[6], u"?"_s);
    const auto syntax = commandsOf<QtZpl::SyntaxCommand>(*document);
    QCOMPARE(syntax.size(), 6);
    for (const auto* command : syntax)
      QCOMPARE(command->sourceSpan.length, qsizetype(4));
    const auto hex = commandsOf<QtZpl::FieldHex>(*document);
    QCOMPARE(hex.size(), 1);
    QCOMPARE(std::get<QtZpl::FieldHex>(hex[0]->payload).indicator, u'%');
    const auto fields = commandsOf<QtZpl::FieldData>(*document);
    QCOMPARE(fields.size(), 2);
    QCOMPARE(*std::get<QtZpl::FieldData>(fields[0]->payload).bytes, QByteArray::fromHex("d096"));
    QCOMPARE(std::get<QtZpl::FieldData>(fields[1]->payload).rawData, u"?10123?d029456"_s);
  }

  void changedDelimiterPreservesGraphicBytes() {
    const QByteArray bytes = QByteArray::fromHex("3b217e00");
    const QString input = u"^XA^CD;^GFB;4;4;2;" + QString::fromLatin1(bytes) + u"^FS^XZ";
    const auto document = QtZpl::parse(input);
    QVERIFY(document.has_value());
    QVERIFY(document->diagnostics().isEmpty());
    const auto graphics = commandsOf<QtZpl::GraphicField>(*document);
    QCOMPARE(graphics.size(), 1);
    const auto& graphic = std::get<QtZpl::GraphicField>(graphics[0]->payload);
    QCOMPARE(graphic.bytesPerRow, 2);
    QCOMPARE(graphic.totalBytes, 4);
    QCOMPARE(graphic.data, bytes);
    QCOMPARE(document->sourceText(graphics[0]->sourceSpan), u"^GFB;4;4;2;" + QString::fromLatin1(bytes));
  }

  void syntaxRequiredCharacter_data() {
    QTest::addColumn<QString>("opcode");
    QTest::addColumn<QChar>("prefix");
    QTest::addColumn<QChar>("character");
    for (const QString& opcode : {u"CC"_s, u"CT"_s, u"CD"_s})
      for (const QChar prefix : {QChar(u'^'), QChar(u'~')})
        for (const QChar character : {QChar(u'!'), QChar(u' '), QChar(u'\r'), QChar(u'\n')}) {
          const auto name = opcode.toLatin1() + '-' + QByteArray::number(prefix.unicode())
            + '-' + QByteArray::number(character.unicode());
          QTest::newRow(name.constData()) << opcode << prefix << character;
        }
  }

  void syntaxRequiredCharacter() {
    QFETCH(QString, opcode);
    QFETCH(QChar, prefix);
    QFETCH(QChar, character);
    QString input = u"^XA"_s + prefix + opcode + character;
    if (opcode == u"CC")
      input += QString{character} + u"FO12,34" + character + u"FS" + character + u"XZ";
    else if (opcode == u"CT")
      input += QString{character} + u"CD;^FO12;34^FS^XZ";
    else
      input += u"^FO12"_s + character + u"34^GFA" + character + u'2' + character + u'2'
        + character + u'1' + character + u"AA55^FS^XZ";
    const auto document = QtZpl::parse(input);
    QVERIFY(document.has_value());
    QVERIFY(document->diagnostics().isEmpty());
    QCOMPARE(document->labels().size(), 1);
    const auto syntax = commandsOf<QtZpl::SyntaxCommand>(*document);
    QVERIFY(!syntax.isEmpty());
    QCOMPARE(std::get<QtZpl::SyntaxCommand>(syntax.front()->payload).character, character);
    QCOMPARE(document->sourceText(syntax.front()->sourceSpan), QString{prefix} + opcode + character);
    const auto origins = commandsOf<QtZpl::FieldOrigin>(*document);
    QCOMPARE(origins.size(), 1);
    QCOMPARE(std::get<QtZpl::FieldOrigin>(origins[0]->payload).x, 12);
    QCOMPARE(std::get<QtZpl::FieldOrigin>(origins[0]->payload).y, 34);
    if (opcode == u"CD") {
      const auto graphics = commandsOf<QtZpl::GraphicField>(*document);
      QCOMPARE(graphics.size(), 1);
      const auto& graphic = std::get<QtZpl::GraphicField>(graphics[0]->payload);
      QCOMPARE(graphic.totalBytes, 2);
      QCOMPARE(graphic.bytesPerRow, 1);
      QCOMPARE(graphic.data, QByteArray("AA55"));
    }
  }

  void syntaxMissingInvalidAndConsumedPrefix() {
    for (const QString& opcode : {u"CC"_s, u"CT"_s, u"CD"_s}) {
      for (const QChar prefix : {QChar(u'^'), QChar(u'~')}) {
        for (const bool preserve : {false, true}) {
          const QString missing = u"^XA"_s + prefix + opcode;
          const auto incomplete = QtZpl::parse(missing, {.preserveUnknownCommands = preserve});
          QVERIFY(incomplete.has_value());
          QCOMPARE(incomplete->source(), missing);
          QCOMPARE(incomplete->diagnostics().size(), 2);
          QCOMPARE(incomplete->diagnostics()[0].code, u"invalid-syntax-character"_s);
          QCOMPARE(incomplete->sourceText(*incomplete->diagnostics()[0].sourceSpan), QString{prefix} + opcode);
          QCOMPARE(incomplete->diagnostics()[1].code, u"unterminated-format"_s);
          QCOMPARE(incomplete->labels().front().sourceSpan(), (QtZpl::SourceSpan{0, missing.size()}));
          QCOMPARE(commandsOf<QtZpl::UnknownCommand>(*incomplete).size(), preserve ? 1 : 0);

          const QString invalid = u"^XA"_s + prefix + opcode + u"Ж^FO12,34^FS^XZ";
          const auto recovered = QtZpl::parse(invalid, {.preserveUnknownCommands = preserve});
          QVERIFY(recovered.has_value());
          QCOMPARE(recovered->diagnostics().size(), 1);
          QCOMPARE(recovered->diagnostics()[0].code, u"invalid-syntax-character"_s);
          const auto origins = commandsOf<QtZpl::FieldOrigin>(*recovered);
          QCOMPARE(origins.size(), 1);
          QCOMPARE(std::get<QtZpl::FieldOrigin>(origins[0]->payload).y, 34);
          QCOMPARE(commandsOf<QtZpl::UnknownCommand>(*recovered).size(), preserve ? 1 : 0);
        }
      }
    }
    // An apparent empty parameter before another command is not empty: the
    // following prefix is the required character and belongs to ^CC.
    const auto consumed = QtZpl::parse(u"^XA^CC^FO12,34^FS^XZ");
    QVERIFY(consumed.has_value());
    QVERIFY(consumed->diagnostics().isEmpty());
    QVERIFY(commandsOf<QtZpl::FieldOrigin>(*consumed).isEmpty());
    const auto syntax = commandsOf<QtZpl::SyntaxCommand>(*consumed);
    QCOMPARE(syntax.size(), 1);
    QCOMPARE(std::get<QtZpl::SyntaxCommand>(syntax[0]->payload).character, u'^');
    QCOMPARE(consumed->sourceText(syntax[0]->sourceSpan), u"^CC^");
    QCOMPARE(consumed->labels().front().sourceSpan(), (QtZpl::SourceSpan{0, consumed->source().size()}));
  }

  void invalidSyntaxLeavesStateAndEvidence() {
    const auto document = QtZpl::parse(u"^XA^CCЖ^FO1,2^XZ^CD");
    QVERIFY(document.has_value());
    QCOMPARE(document->labels().size(), 1);
    QCOMPARE(commandsOf<QtZpl::FieldOrigin>(*document).size(), 1);
    QCOMPARE(document->diagnostics().size(), 2);
    QCOMPARE(document->diagnostics()[0].code, u"invalid-syntax-character"_s);
    QCOMPARE(document->diagnostics()[0].labelIndex, std::optional<int>{0});
    QVERIFY(!document->diagnostics()[1].labelIndex.has_value());
    const auto unknown = commandsOf<QtZpl::UnknownCommand>(*document);
    QCOMPARE(unknown.size(), 1);
    QCOMPARE(document->sourceText(unknown[0]->sourceSpan), u"^CCЖ");
  }

  void exactResourceLimits() {
    const QString input = u"^XA^FH^FD_41^FS^XZ"_s;
    QtZpl::ParseOptions options;
    options.maxSourceLength = input.size();
    options.maxCommands = 5;
    options.maxLabels = 1;
    options.maxDiagnostics = 0;
    QVERIFY(QtZpl::parse(input, options).has_value());
    --options.maxSourceLength;
    auto result = QtZpl::parse(input, options);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, u"parse-source-limit"_s);
    ++options.maxSourceLength;
    --options.maxCommands;
    result = QtZpl::parse(input, options);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, u"parse-command-limit"_s);
    QCOMPARE(result.error().offset, input.indexOf(u"^XZ"));
    ++options.maxCommands;
    options.maxLabels = 0;
    result = QtZpl::parse(input, options);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, u"parse-label-limit"_s);
    options = {};
    options.maxDiagnostics = 1;
    QVERIFY(QtZpl::parse(u"^XA^ZZ^XZ", options).has_value());
    result = QtZpl::parse(u"^XA^ZZ^ZZ^XZ", options);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, u"parse-diagnostic-limit"_s);
    options = {};
    options.maxCommands = 1;
    result = QtZpl::parse(u"^FO1,2^FO3,4", options);
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, u"parse-command-limit"_s);
    QVERIFY(QtZpl::parse(u"", {.maxSourceLength = 0, .maxCommands = 0,
      .maxLabels = 0, .maxDiagnostics = 0}).has_value());
  }

  void invalidLimitsAndCancellation() {
    for (const auto member : {&QtZpl::ParseOptions::maxSourceLength, &QtZpl::ParseOptions::maxCommands,
        &QtZpl::ParseOptions::maxLabels, &QtZpl::ParseOptions::maxDiagnostics}) {
      QtZpl::ParseOptions options;
      options.*member = -1;
      const auto result = QtZpl::parse(u"", options);
      QVERIFY(!result.has_value());
      QCOMPARE(result.error().code, u"invalid-parse-options"_s);
    }
    std::stop_source stop;
    stop.request_stop();
    const auto result = QtZpl::parse(u"^XA^XZ", {.stopToken = stop.get_token()});
    QVERIFY(!result.has_value());
    QCOMPARE(result.error().code, u"operation-cancelled"_s);
  }

  void concurrentSnapshotsOwnTheirSource() {
    const QString input = u"prefix^xa^ci28^fh^fd_D0_96\U0001f600^fs^xz tail"_s;
    std::vector<std::future<std::expected<QtZpl::Document, QtZpl::ParseError>>> futures;
    for (int i = 0; i < 8; ++i)
      futures.push_back(std::async(std::launch::async, [input] { return QtZpl::parse(input); }));
    for (auto& future : futures) {
      const auto document = future.get();
      QVERIFY(document.has_value());
      QCOMPARE(document->source(), input);
      QCOMPARE(document->labels().size(), 1);
      const auto fields = commandsOf<QtZpl::FieldData>(*document);
      QCOMPARE(fields.size(), 1);
      const auto& field = std::get<QtZpl::FieldData>(fields[0]->payload);
      QCOMPARE(document->sourceText(field.sourceSpan), u"_D0_96\U0001f600");
      QCOMPARE(*field.bytes, QByteArray::fromHex("d096f09f9880"));
    }
  }
};

QTEST_APPLESS_MAIN(ParserContractTest)
#include "test_parser_contract.moc"
