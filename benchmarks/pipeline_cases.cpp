#include "benchmark.hpp"

#include <QtZpl/qtzpl.hpp>

#include <QtCore/QDir>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDataStream>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonParseError>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace Bench {
namespace {
using namespace Qt::StringLiterals;

[[noreturn]] void fail(const QString& name, const QString& message) {
  throw std::runtime_error((name + u": " + message).toStdString());
}

QByteArray readFile(const QString& path) {
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) fail(path, file.errorString());
  return file.readAll();
}

void verifyDiagnostics(const QString& name, const QList<QtZpl::Diagnostic>& diagnostics) {
  for (const auto& diagnostic : diagnostics) {
    if (diagnostic.severity == QtZpl::Severity::Error)
      fail(name, diagnostic.code + u": " + diagnostic.message);
  }
}

QtZpl::Document prepareDocument(const QString& name, const QString& input, int labelCount) {
  auto result = QtZpl::parse(input);
  if (!result) fail(name, result.error().code + u": " + result.error().message);
  if (result->labels().size() != labelCount) fail(name, u"Unexpected parsed label count"_s);
  verifyDiagnostics(name, result->diagnostics());
  return std::move(*result);
}

void verifyRender(const QString& name, const QtZpl::RenderResult& result,
                  const QtZpl::RenderOptions& options, int labelCount) {
  if (result.labels.size() != labelCount) fail(name, u"Unexpected rendered label count"_s);
  verifyDiagnostics(name, result.diagnostics);
  for (const auto& image : result.labels) {
    if (image.isNull()) fail(name, u"Null rendered image"_s);
    if ((options.width > 0 && image.width() != options.width)
        || (options.height > 0 && image.height() != options.height))
      fail(name, u"Unexpected rendered image dimensions"_s);
  }
}

// Sample results cheaply: traversing every command or pixel would distort the
// operation being measured. Full semantic and pixel checks belong to the tests.
quint64 documentChecksum(const QtZpl::Document& document) {
  quint64 value = static_cast<quint64>(document.labels().size() + document.diagnostics().size());
  for (const auto& label : document.labels()) {
    value += static_cast<quint64>(label.commands().size());
    if (!label.commands().isEmpty())
      value += static_cast<quint64>(label.commands().back().source.size());
  }
  return value;
}

quint64 renderChecksum(const QtZpl::RenderResult& result) {
  quint64 value = static_cast<quint64>(result.labels.size() + result.diagnostics.size());
  for (const auto& image : result.labels) {
    value += static_cast<quint64>(image.width()) * static_cast<quint64>(image.height());
    if (!image.isNull()) value += image.pixel(image.width() / 2, image.height() / 2);
  }
  return value;
}

void writeDiagnostics(QDataStream& stream, const QList<QtZpl::Diagnostic>& diagnostics) {
  stream << qint64(diagnostics.size());
  for (const auto& diagnostic : diagnostics)
    stream << qint32(diagnostic.severity) << diagnostic.code << diagnostic.message
           << qint64(diagnostic.offset) << diagnostic.command;
}

QString documentFingerprint(const QtZpl::Document& document) {
  QByteArray bytes;
  QDataStream stream(&bytes, QIODevice::WriteOnly);
  stream.setVersion(QDataStream::Qt_6_0);
  stream << qint64(document.labels().size());
  for (const auto& label : document.labels()) {
    stream << qint32(label.width()) << qint32(label.height())
           << qint32(label.homeX()) << qint32(label.homeY())
           << qint64(label.commands().size());
    for (const auto& command : label.commands()) {
      stream << qint64(command.offset) << command.source << quint64(command.payload.index());
      // Also hash decoded data: identical source must not hide a changed ^FH
      // result or a changed graphics/barcode payload in parser experiments.
      std::visit([&](const auto& payload) {
        using T = std::decay_t<decltype(payload)>;
        if constexpr (std::is_same_v<T, QtZpl::FieldData>) stream << payload.data;
        else if constexpr (std::is_same_v<T, QtZpl::GraphicField>)
          stream << payload.compression << qint32(payload.totalBytes) << qint32(payload.bytesUsed)
                 << qint32(payload.bytesPerRow) << payload.data;
        else if constexpr (std::is_same_v<T, QtZpl::Barcode>)
          stream << payload.symbology << qint32(payload.orientation) << payload.parameters << payload.data;
      }, command.payload);
    }
  }
  writeDiagnostics(stream, document.diagnostics());
  return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QString renderFingerprint(const QtZpl::RenderResult& result) {
  QCryptographicHash hash(QCryptographicHash::Sha256);
  QByteArray metadata;
  QDataStream stream(&metadata, QIODevice::WriteOnly);
  stream.setVersion(QDataStream::Qt_6_0);
  stream << qint64(result.labels.size());
  for (const auto& image : result.labels) {
    stream << qint32(image.width()) << qint32(image.height()) << qint32(image.format());
    // The renderer produces ARGB32 with no row padding. Hash the complete
    // raster only during preflight, never inside the measured operation.
    for (int y = 0; y < image.height(); ++y)
      hash.addData(QByteArrayView(reinterpret_cast<const char*>(image.constScanLine(y)),
                                 image.bytesPerLine()));
  }
  writeDiagnostics(stream, result.diagnostics);
  hash.addData(metadata);
  return QString::fromLatin1(hash.result().toHex());
}

void addParse(Cases& cases, const QString& name, const QString& description,
              const QString& input, qint64 inputBytes, const QtZpl::Document& document,
              qint64 units, const QString& unit) {
  cases.push_back({u"parse/" + name, u"parse"_s, description, inputBytes, units, unit,
    static_cast<int>(document.diagnostics().size()),
    [input] {
      auto result = QtZpl::parse(input);
      if (!result) fail(u"parse"_s, result.error().message);
      return documentChecksum(*result);
    }});
  cases.back().outputFingerprint = documentFingerprint(document);
}

void addRender(Cases& cases, const QString& name, const QString& description,
               const QString& input, qint64 inputBytes, const QtZpl::Document& document,
               const QtZpl::RenderOptions& options, qint64 units, const QString& unit,
               bool endToEnd = false) {
  auto preflight = QtZpl::render(document, options);
  if (!preflight) fail(name, preflight.error().code + u": " + preflight.error().message);
  verifyRender(name, *preflight, options, static_cast<int>(document.labels().size()));
  const int diagnostics = static_cast<int>(preflight->diagnostics.size());
  cases.push_back({u"render/" + name, u"render"_s, description, inputBytes, units, unit,
    diagnostics, [document, options] {
      auto result = QtZpl::render(document, options);
      if (!result) fail(u"render"_s, result.error().message);
      return renderChecksum(*result);
    }});
  cases.back().outputFingerprint = renderFingerprint(*preflight);
  if (endToEnd) {
    auto combined = QtZpl::render(input, {}, options);
    if (!combined) fail(name, combined.error().code + u": " + combined.error().message);
    verifyRender(name, *combined, options, static_cast<int>(document.labels().size()));
    cases.push_back({u"end-to-end/" + name, u"end-to-end"_s, description, inputBytes,
      units, unit, static_cast<int>(combined->diagnostics.size()), [input, options] {
        auto result = QtZpl::render(input, {}, options);
        if (!result) fail(u"end-to-end"_s, result.error().message);
        return renderChecksum(*result);
      }});
    cases.back().outputFingerprint = renderFingerprint(*combined);
  }
}

QtZpl::RenderOptions standardOptions() {
  QtZpl::RenderOptions options;
  options.width = 812;
  options.height = 1218;
  return options;
}

void addSyntheticParse(Cases& cases, const QString& name, const QString& description,
                       const QString& input, qint64 units = 1,
                       const QString& unit = u"label"_s, int labelCount = 1) {
  const auto document = prepareDocument(name, input, labelCount);
  addParse(cases, u"synthetic/" + name, description, input, input.toUtf8().size(),
           document, units, unit);
}

void addSyntheticRender(Cases& cases, const QString& name, const QString& description,
                        const QString& input, const QtZpl::RenderOptions& options,
                        qint64 units = 1, const QString& unit = u"label"_s) {
  const auto document = prepareDocument(name, input, 1);
  addRender(cases, u"synthetic/" + name, description, input, input.toUtf8().size(),
            document, options, units, unit);
}

QString textFields(QChar font, int count, bool unique) {
  QString input = u"^XA"_s;
  for (int i = 0; i < count; ++i) {
    const QString text = unique
      ? u"ITEM %1 ABCDEFGHIJK"_s.arg(i, 4, 10, u'0')
      : u"ITEM 0000 ABCDEFGHIJK"_s;
    input += u"^FO%1,%2^A%3N,24,20^FD%4^FS"_s
      .arg(10 + (i % 4) * 200).arg(10 + (i / 4) * 42).arg(font).arg(text);
  }
  return input + u"^XZ";
}

QString graphicInput(unsigned char byte) {
  constexpr int bytes = 64 * 512;
  const QByteArray data(bytes, static_cast<char>(byte));
  return u"^XA^FO10,10^GFA,%1,%1,64,%2^FS^XZ"_s.arg(bytes)
    .arg(QString::fromLatin1(data.toHex()));
}

void addSyntheticCases(Cases& cases) {
  const auto options = standardOptions();
  for (const int dpi : {203, 300, 600}) {
    auto blankOptions = options;
    blankOptions.dpi = dpi;
    blankOptions.width = dpi * 4;
    blankOptions.height = dpi * 6;
    const auto name = u"blank-%1x%2"_s.arg(blankOptions.width).arg(blankOptions.height);
    addSyntheticRender(cases, name, u"Allocate and fill a blank four-by-six-inch label"_s,
      u"^XA^XZ"_s, blankOptions);
  }
  for (const QChar font : {QChar(u'0'), QChar(u'A')}) {
    for (const int count : {1, 100}) {
      const auto name = u"font%1-%2-repeated"_s.arg(font).arg(count);
      addSyntheticRender(cases, name, u"Repeated 21-character text fields, 24x20 dots"_s,
        textFields(font, count, false), options, count, u"field"_s);
    }
    addSyntheticRender(cases, u"font%1-100-unique"_s.arg(font),
      u"100 distinct 21-character text fields, 24x20 dots"_s,
      textFields(font, 100, true), options, 100, u"field"_s);
  }

  for (const bool spaced : {false, true}) {
    const QString text = spaced ? QString(u"LABEL WORD "_s).repeated(100)
                                : QString(1100, u'W');
    addSyntheticRender(cases, spaced ? u"font0-block-words"_s : u"font0-block-long-word"_s,
      u"1100 characters in a 600-dot Font 0 block, at most 40 lines"_s,
      u"^XA^FO10,10^A0N,24,20^FB600,40,0,L,0^FD" + text + u"^FS^XZ", options);
  }
  addSyntheticRender(cases, u"font0-block-long-word-275"_s,
    u"275 characters in a 600-dot Font 0 block, at most 40 lines"_s,
    u"^XA^FO10,10^A0N,24,20^FB600,40,0,L,0^FD" + QString(275, u'W') + u"^FS^XZ", options);

  for (const unsigned char byte : {static_cast<unsigned char>(0),
                                  static_cast<unsigned char>(0xff),
                                  static_cast<unsigned char>(0xaa)}) {
    const auto name = byte == 0 ? u"gf-512-empty"_s
      : byte == 0xff ? u"gf-512-solid"_s : u"gf-512-stripes"_s;
    const auto input = graphicInput(byte);
    addSyntheticRender(cases, name, u"Uncompressed ASCII-hex ^GF, 512x512 dots"_s,
      input, options);
    if (byte == 0xaa)
      addSyntheticParse(cases, name, u"65,536 hexadecimal characters in ^GF"_s, input);
  }

  for (const int bytes : {32, 512}) {
    const QString payload = QString(u"Abcd0123"_s).repeated(bytes / 8);
    addSyntheticRender(cases, u"qr-%1-bytes"_s.arg(bytes),
      u"QR Model 2, medium ECC, automatic mask, 2-dot modules"_s,
      u"^XA^FO10,10^BQN,2,2^FDMA," + payload + u"^FS^XZ", options);
  }
  addSyntheticRender(cases, u"maxicode-ups"_s,
    u"One MaxiCode mode 2 with the committed UPS fixture payload"_s,
    u"^XA^FO10,10^BD2^FH_^FD000000000000000[)>_1E01_1D961Z00000001_1DUPSN_1D00A00A"
    u"_1E07Y+0*0A.AA'AA#A0A%'_0DAAA0.00_1C*0AAA'A_1C0AA000$&A_0D_1E_04^FS^XZ"_s,
    options);

  for (const int count : {100, 1000}) {
    addSyntheticParse(cases, u"fields-%1"_s.arg(count),
      u"Parse field origin, font, data and separator for each field"_s,
      textFields(u'0', count, true), count, u"field"_s);
  }
  for (const bool commas : {false, true}) {
    const QString payload = commas ? QString(u"a,"_s).repeated(32 * 1024)
                                  : QString(64 * 1024, u'a');
    addSyntheticParse(cases, commas ? u"fd-64k-commas"_s : u"fd-64k-plain"_s,
      u"One 65,536-character ^FD; commas belong to field data"_s,
      u"^XA^FD" + payload + u"^FS^XZ");
  }
  addSyntheticParse(cases, u"fh-16k-bytes"_s,
    u"16,384 hexadecimal byte escapes in one ^FH/^FD field"_s,
    u"^XA^FH^FD" + QString(u"_41"_s).repeated(16 * 1024) + u"^FS^XZ");
  addSyntheticParse(cases, u"labels-100"_s,
    u"100 separate one-field labels in one input stream"_s,
    textFields(u'0', 1, false).repeated(100), 100, u"label"_s, 100);

  for (const int count : {1, 100}) {
    QString input = u"^XA^BY1,3,8"_s;
    for (int i = 0; i < count; ++i)
      input += u"^FO10,%1^BCN,8,N,N,N,A^FD01234567890123456789^FS"_s.arg(i * 12);
    input += u"^XZ";
    addSyntheticRender(cases, u"code128-%1"_s.arg(count),
      u"Numeric Code 128 auto subset, 20 digits, no interpretation"_s,
      input, options, count, u"barcode"_s);
  }
}

} // namespace

void addPipelineCases(Cases& cases, const QString& corpusDir) {
  const QDir directory(corpusDir);
  QJsonParseError error;
  const auto manifest = QJsonDocument::fromJson(readFile(directory.filePath(u"manifest.json"_s)), &error);
  if (error.error != QJsonParseError::NoError || !manifest.isObject())
    fail(corpusDir, u"Invalid manifest: " + error.errorString());
  const auto examples = manifest.object().value(u"examples"_s).toArray();
  if (examples.isEmpty()) fail(corpusDir, u"Manifest contains no examples"_s);
  for (const auto& entry : examples) {
    const auto object = entry.toObject();
    const auto fixture = object.value(u"fixture"_s).toString();
    const auto name = u"corpus/" + fixture.chopped(4);
    const QByteArray bytes = readFile(directory.filePath(fixture));
    const QString input = QString::fromUtf8(bytes);
    const int pages = object.value(u"pages"_s).toInt();
    if (pages <= 0) fail(name, u"Manifest has no valid expected page count"_s);
    QtZpl::RenderOptions options;
    options.dpi = object.value(u"dpi"_s).toInt();
    options.width = object.value(u"widthDots"_s).toInt();
    options.height = object.value(u"heightDots"_s).toInt();
    options.ignoreLabelHome = object.value(u"ignoreLabelHome"_s).toBool();
    if (options.width <= 0 || options.height <= 0)
      fail(name, u"Manifest has no valid raster dimensions"_s);
    const auto document = prepareDocument(name, input, pages);
    const auto description = u"Committed demo fixture; %1 page(s), %2x%3 dots, %4 DPI"_s
      .arg(pages).arg(options.width).arg(options.height).arg(options.dpi);
    addParse(cases, name, description, input, bytes.size(), document, pages, u"label"_s);
    addRender(cases, name, description, input, bytes.size(), document, options,
      pages, u"label"_s, true);
  }
  addSyntheticCases(cases);
}

} // namespace Bench
