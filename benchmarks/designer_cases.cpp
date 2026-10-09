#include "benchmark.hpp"

#include <QtZpl/qtzpl.hpp>
#include <QtCore/QCryptographicHash>
#include <QtCore/QDataStream>
#include <QtCore/QIODevice>
#include <memory>
#include <stdexcept>

namespace Bench {
namespace {
using namespace Qt::StringLiterals;

[[noreturn]] void fail(const QString& message) {
  throw std::runtime_error((u"designer benchmark: " + message).toStdString());
}

template<typename T, typename E>
const T& checked(const std::expected<T, E>& result) {
  if (!result) fail(result.error().message);
  return *result;
}

void noDiagnostics(const QList<QtZpl::Diagnostic>& diagnostics) {
  if (!diagnostics.isEmpty()) fail(diagnostics.front().code + u": " + diagnostics.front().message);
}

quint64 imageChecksum(const QImage& image) {
  if (image.isNull()) fail(u"Null image"_s);
  return quint64(image.width()) * image.height() + image.pixel(image.width() / 2, image.height() / 2);
}

quint64 fieldChecksum(const QList<QtZpl::FieldInfo>& fields) {
  return quint64(fields.size()) + (fields.isEmpty() ? 0 : quint64(fields.back().id));
}

void writeFields(QDataStream& out, const QList<QtZpl::FieldInfo>& fields) {
  out << qint64(fields.size());
  for (const auto& field : fields) {
    const auto& state = field.settings;
    out << qint64(field.id) << qint32(field.labelIndex) << qint64(field.commandIndex)
        << qint64(field.sourceSpan.start) << qint64(field.sourceSpan.length)
        << qint64(field.payloadSpan.start) << qint64(field.payloadSpan.length)
        << qint32(field.kind) << field.hasUnknownCommands
        << state.position << state.baseline << qint32(state.justification)
        << state.font.font << qint32(state.font.orientation) << state.font.height << state.font.width
        << state.defaultFont.font << state.defaultFont.height << state.defaultFont.width
        << qint32(state.fieldDirection) << state.barcodeDefaults.moduleWidth
        << state.barcodeDefaults.wideToNarrowRatio << state.barcodeDefaults.height
        << state.characterSet << state.hexIndicator << state.labelHome
        << state.labelShift << state.labelTop << state.reverse
        << qint32(state.printOrientation) << state.mirror;
    out << state.barcode.has_value();
    if (state.barcode) out << state.barcode->symbology << qint32(state.barcode->orientation) << state.barcode->parameters;
    out << state.block.has_value();
    if (state.block) out << state.block->width << state.block->maxLines << state.block->lineSpacing
                         << qint32(state.block->justification) << state.block->hangingIndent;
    out << state.parameter.direction << state.parameter.spacing;
  }
}

template<typename Write>
QString fingerprint(Write write) {
  QByteArray bytes;
  QDataStream stream(&bytes, QIODevice::WriteOnly);
  stream.setVersion(QDataStream::Qt_6_0);
  write(stream);
  return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

QString renderFingerprint(qsizetype outputCount, const QImage& selected,
                          const QList<QtZpl::FieldGeometry>& fields = {}) {
  return fingerprint([&](QDataStream& out) {
    // Hash one common selected page, while retaining the actual output count.
    out << qint64(outputCount) << selected.size() << qint32(selected.format());
    for (int y = 0; y < selected.height(); ++y)
      out.writeRawData(reinterpret_cast<const char*>(selected.constScanLine(y)), selected.bytesPerLine());
    out << qint64(fields.size());
    for (const auto& field : fields) {
      writeFields(out, {field.field});
      out << field.logicalBounds << field.paintBounds << field.clippedBounds
          << field.anchor << field.baseline << field.labelTransform << qint32(field.status);
    }
  });
}
}

void addDesignerCases(Cases& cases) {
  constexpr int pageCount = 8;
  constexpr int selectedPage = 5;
  QString source;
  for (int page = 0; page < pageCount; ++page)
    source += u"^XA^PW812^LL406^CI28^FO20,20^A0N,32,24^FDPage %1 Жé^FS"
              u"^FT20,125^A0N,24,18^FB350,2,2,L^FDDesigner API\\&baseline text^FS"
              u"^FO450,20^BQN,2,4^FDMA,https://example.invalid/%1^FS"
              u"^FO20,210^BY2,3,65^BCN,65,Y,N,N,A^FD12345678%1^FS"
              u"^FO420,210^GB320,110,3^FS^XZ"_s.arg(page);
  const auto parsed = QtZpl::parse(source);
  noDiagnostics(checked(parsed).diagnostics());
  const auto document = std::make_shared<const QtZpl::Document>(*parsed);
  const qint64 sourceBytes = source.toUtf8().size();
  const QtZpl::RenderOptions geometryOptions{.collectFieldGeometry = true};
  const auto full = QtZpl::render(*document);
  const auto selected = QtZpl::renderLabel(*document, selectedPage);
  const auto geometry = QtZpl::renderLabel(*document, selectedPage, geometryOptions);
  const auto allFields = QtZpl::analyze(*document);
  const auto selectedFields = QtZpl::analyzeLabel(*document, selectedPage);
  noDiagnostics(checked(full).diagnostics);
  noDiagnostics(checked(selected).diagnostics);
  noDiagnostics(checked(geometry).diagnostics);
  noDiagnostics(checked(allFields).diagnostics);
  noDiagnostics(checked(selectedFields).diagnostics);
  if (full->labels.size() != pageCount || selected->labelIndex != selectedPage
      || full->labels[selectedPage] != selected->image || selected->image != geometry->image
      || allFields->fields.size() != pageCount * 5 || selectedFields->fields.size() != 5
      || geometry->fields.size() != 5)
    fail(u"Page selection or field metadata validation failed"_s);

  cases.push_back({.name = u"designer/render-all"_s, .category = u"designer"_s,
    .description = u"Render all eight labels from one shared parsed snapshot; fingerprint includes eight outputs and page 5 pixels"_s,
    .inputBytes = sourceBytes, .units = pageCount, .unit = u"label"_s,
    .run = [document] { const auto result = QtZpl::render(*document);
      return quint64(checked(result).labels.size()) + imageChecksum(result->labels[selectedPage]); },
    .outputFingerprint = renderFingerprint(full->labels.size(), full->labels[selectedPage])});
  cases.push_back({.name = u"designer/render-label"_s, .category = u"designer"_s,
    .description = u"Render selected page 5 from the same eight-label parsed snapshot"_s,
    .inputBytes = sourceBytes, .unit = u"label"_s,
    .run = [document] { const auto result = QtZpl::renderLabel(*document, selectedPage);
      return imageChecksum(checked(result).image) + quint64(result->labelIndex); },
    .outputFingerprint = renderFingerprint(1, selected->image)});
  cases.push_back({.name = u"designer/render-label-geometry"_s, .category = u"designer"_s,
    .description = u"Selected page with field bounds from the same render; pixel and geometry fingerprint"_s,
    .inputBytes = sourceBytes, .unit = u"label"_s,
    .run = [document, geometryOptions] { const auto result = QtZpl::renderLabel(*document, selectedPage, geometryOptions);
      return imageChecksum(checked(result).image) + quint64(result->fields.size()); },
    .outputFingerprint = renderFingerprint(1, geometry->image, geometry->fields)});
  cases.push_back({.name = u"designer/analyze-all"_s, .category = u"designer"_s,
    .description = u"Interpret effective settings of all 40 fields without label raster allocation"_s,
    .inputBytes = sourceBytes, .units = allFields->fields.size(), .unit = u"field"_s,
    .run = [document] { const auto result = QtZpl::analyze(*document); return fieldChecksum(checked(result).fields); },
    .outputFingerprint = fingerprint([&](QDataStream& out) { writeFields(out, allFields->fields); })});
  cases.push_back({.name = u"designer/analyze-label"_s, .category = u"designer"_s,
    .description = u"Interpret the five fields of selected page 5 without label raster allocation"_s,
    .inputBytes = sourceBytes, .units = selectedFields->fields.size(), .unit = u"field"_s,
    .run = [document] { const auto result = QtZpl::analyzeLabel(*document, selectedPage); return fieldChecksum(checked(result).fields); },
    .outputFingerprint = fingerprint([&](QDataStream& out) { writeFields(out, selectedFields->fields); })});

  const QList<std::pair<QString, QString>> graphics{
    {u"compressed"_s, u"^GFA,8,8,2,HA,:!,"_s},
    {u"z64"_s, u"^GFA,10,2,1,:Z64:eJxbFQoAAasBAA==:B023"_s}
  };
  for (const auto& [name, command] : graphics) {
    const auto graphicParsed = QtZpl::parse(u"^XA^PW812^LL406^FO20,20" + command + u"^FS^XZ");
    noDiagnostics(checked(graphicParsed).diagnostics());
    const auto graphicDocument = std::make_shared<const QtZpl::Document>(*graphicParsed);
    std::optional<QtZpl::GraphicField> graphic;
    for (const auto& item : graphicDocument->labels().front().commands())
      if (const auto* value = std::get_if<QtZpl::GraphicField>(&item.payload)) graphic = *value;
    if (!graphic) fail(u"Missing graphic field"_s);
    const auto decoded = QtZpl::decodeGraphic(*graphic);
    checked(decoded);
    const auto graphicRendered = QtZpl::renderLabel(*graphicDocument, 0);
    noDiagnostics(checked(graphicRendered).diagnostics);
    cases.push_back({.name = u"designer/decode-" + name, .category = u"designer"_s,
      .description = u"Decode GF to packed bytes; no raster allocation"_s,
      .inputBytes = graphic->data.size(), .units = decoded->bytes.size(), .unit = u"decoded byte"_s,
      .run = [graphic = *graphic] { const auto result = QtZpl::decodeGraphic(graphic);
        const auto& bytes = checked(result).bytes;
        return quint64(bytes.size()) + static_cast<unsigned char>(bytes.front()); },
      .outputFingerprint = fingerprint([&](QDataStream& out) { out << decoded->size << decoded->bytesPerRow << decoded->bytes; })});
    cases.push_back({.name = u"designer/render-" + name + u"-graphic", .category = u"designer"_s,
      .description = u"Decode the same GF through an 812x406 label render"_s,
      .inputBytes = graphicDocument->source().toUtf8().size(), .unit = u"label"_s,
      .run = [graphicDocument] { const auto result = QtZpl::renderLabel(*graphicDocument, 0);
        return imageChecksum(checked(result).image); },
      .outputFingerprint = renderFingerprint(1, graphicRendered->image)});
  }
}
} // namespace Bench
