#include "benchmark.hpp"

#include "aztec_encoder.hpp"
#include "barcode_encoders.hpp"
#include "databar_encoder.hpp"
#include "linear_encoders.hpp"
#include "maxicode_encoder.hpp"
#include "micropdf417_encoder.hpp"
#include "pdf417_encoder.hpp"
#include <QtZpl/qtzpl.hpp>

#include <QtCore/QCryptographicHash>
#include <QtCore/QDataStream>
#include <QtCore/QIODevice>
#include <algorithm>
#include <expected>
#include <stdexcept>

namespace Bench {
namespace {
using namespace Qt::StringLiterals;
namespace Encoders = QtZpl::BarcodeEncoders;

[[noreturn]] void fail(const QString& name, const QString& message) {
    throw std::runtime_error((name + u": " + message).toStdString());
}

template<typename T>
const T& checked(const std::expected<T, QString>& result) {
    if (!result) fail(u"extension encoder"_s, result.error());
    return *result;
}

quint64 summary(const QVector<bool>& modules) {
    if (modules.isEmpty()) fail(u"extension encoder"_s, u"Empty modules"_s);
    return quint64(modules.size()) * 131 + quint64(modules.front()) * 17
           + quint64(modules[modules.size() / 2]) * 7 + quint64(modules.back());
}
quint64 summary(const Encoders::Matrix& matrix) {
    if (matrix.width <= 0 || matrix.height <= 0 || matrix.modules.size() != matrix.width * matrix.height)
        fail(u"extension encoder"_s, u"Invalid matrix geometry"_s);
    return summary(matrix.modules) + quint64(matrix.width) * 65537 + quint64(matrix.height);
}
quint64 summary(const Encoders::Linear::Symbol& symbol) { return summary(symbol.modules); }
quint64 summary(const Encoders::MaxiCode::Symbol& symbol) { return summary(symbol.grid); }
quint64 summary(const Encoders::Pdf417::Symbol& symbol) { return summary(symbol.matrix); }

void serialize(QDataStream& stream, const QVector<bool>& modules) {
    if (!std::ranges::any_of(modules, [](bool value) { return value; }))
        fail(u"extension encoder"_s, u"No dark modules"_s);
    stream << qint64(modules.size());
    for (const bool module : modules) stream << quint8(module);
}
void serialize(QDataStream& stream, const Encoders::Matrix& matrix) {
    stream << qint32(matrix.width) << qint32(matrix.height);
    serialize(stream, matrix.modules);
}
void serialize(QDataStream& stream, const Encoders::Linear::Symbol& symbol) {
    serialize(stream, symbol.modules);
    stream << symbol.text << symbol.checkText;
}
void serialize(QDataStream& stream, const Encoders::MaxiCode::Symbol& symbol) {
    serialize(stream, symbol.grid);
    stream << symbol.codewords;
}
void serialize(QDataStream& stream, const Encoders::Pdf417::Symbol& symbol) {
    serialize(stream, symbol.matrix);
    stream << symbol.dataCodewords << symbol.allCodewords;
}
void serialize(QDataStream& stream, const QtZpl::Document& document);
void serialize(QDataStream& stream, const QtZpl::RenderResult& result);
template<typename T>
QString fingerprint(const T& value) {
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    serialize(stream, value);
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

template<typename Encode>
void addNative(Cases& cases, const QString& name, const QString& description,
               qint64 inputBytes, Encode encode) {
    const auto prepared = encode();
    summary(checked(prepared));
    cases.push_back({.name = u"algorithm/extensions/" + name,
        .category = u"algorithms"_s, .description = description, .inputBytes = inputBytes,
        .run = [encode] { const auto result = encode(); return summary(checked(result)); },
        .outputFingerprint = fingerprint(checked(prepared))});
}

void strictDiagnostics(const QString& name, const QList<QtZpl::Diagnostic>& diagnostics) {
    // New supported workloads must not time a warning/error placeholder.
    if (!diagnostics.isEmpty())
        fail(name, diagnostics.front().code + u": " + diagnostics.front().message);
}

quint64 summary(const QtZpl::Document& document) {
    return quint64(document.labels().size()) * 131 + quint64(document.labels().front().commands().size());
}
quint64 summary(const QtZpl::RenderResult& result) {
    const auto& image = result.labels.front();
    return quint64(image.width()) * quint64(image.height()) + image.pixel(image.width() / 2, image.height() / 2);
}
void serialize(QDataStream& stream, const QtZpl::Document& document) {
    stream << qint64(document.labels().size());
    for (const auto& label : document.labels()) {
        stream << qint32(label.width()) << qint32(label.height()) << qint32(label.homeX()) << qint32(label.homeY());
        for (const auto& command : label.commands())
            stream << qint64(command.offset) << command.source << quint64(command.payload.index());
    }
}
void serialize(QDataStream& stream, const QtZpl::RenderResult& result) {
    stream << qint64(result.labels.size());
    for (const auto& image : result.labels) {
        stream << qint32(image.width()) << qint32(image.height()) << qint32(image.format());
        for (int y = 0; y < image.height(); ++y)
            stream.writeRawData(reinterpret_cast<const char*>(image.constScanLine(y)), image.bytesPerLine());
    }
}

void verifyRender(const QString& name, const QtZpl::RenderResult& result) {
    strictDiagnostics(name, result.diagnostics);
    if (result.labels.size() != 1 || result.labels.front().size() != QSize(812, 406))
        fail(name, u"Unexpected rendered image dimensions/count"_s);
    const auto& image = result.labels.front();
    bool ink = false;
    for (int y = 0; y < image.height() && !ink; ++y)
        for (int x = 0; x < image.width(); ++x)
            if (qGray(image.pixel(x, y)) < 128) { ink = true; break; }
    if (!ink) fail(name, u"Rendered label has no ink"_s);
}

void addPipeline(Cases& cases, const QString& name, const QString& description, const QString& fields) {
    const QString input = u"^XA^PW812^LL406" + fields + u"^XZ";
    const auto document = QtZpl::parse(input);
    if (!document) fail(name, document.error().message);
    strictDiagnostics(name, document->diagnostics());
    if (document->labels().size() != 1) fail(name, u"Unexpected parsed label count"_s);
    const auto rendered = QtZpl::render(*document);
    if (!rendered) fail(name, rendered.error().message);
    verifyRender(name, *rendered);
    const auto combined = QtZpl::render(input);
    if (!combined) fail(name, combined.error().message);
    verifyRender(name, *combined);
    const auto renderHash = fingerprint(*rendered);
    if (renderHash != fingerprint(*combined)) fail(name, u"Pipeline stages disagree"_s);
    const qint64 bytes = input.toUtf8().size();
    cases.push_back({u"parse/extensions/" + name, u"parse"_s, description, bytes, 1, u"label"_s, 0,
        [input] { const auto result = QtZpl::parse(input); if (!result) fail(u"parse"_s, result.error().message);
                  return summary(*result); }, fingerprint(*document)});
    cases.push_back({u"render/extensions/" + name, u"render"_s, description, bytes, 1, u"label"_s, 0,
        [document = *document] { const auto result = QtZpl::render(document);
            if (!result) fail(u"render"_s, result.error().message); return summary(*result); }, renderHash});
    cases.push_back({u"end-to-end/extensions/" + name, u"end-to-end"_s, description, bytes, 1, u"label"_s, 0,
        [input] { const auto result = QtZpl::render(input); if (!result) fail(u"end-to-end"_s, result.error().message);
                  return summary(*result); }, renderHash});
}
} // namespace

void addExtensionCases(Cases& cases) {
    struct LinearCase { QString name; QString command; QString data; };
    for (const auto& spec : {
             LinearCase{u"ean8"_s,u"B8"_s,u"9638507"_s},
             LinearCase{u"upce"_s,u"B9"_s,u"0425261"_s},
             LinearCase{u"code93"_s,u"BA"_s,u"ABC-12345"_s},
             LinearCase{u"upca"_s,u"BU"_s,u"03600029145"_s},
             LinearCase{u"interleaved2of5"_s,u"B2"_s,u"12345678"_s},
             LinearCase{u"industrial2of5"_s,u"BI"_s,u"12345678"_s}}) {
        addNative(cases, spec.name, u"Complete linear modules and normalized caption"_s, spec.data.size(),
                  [spec] { return Encoders::Linear::encode(spec.command, spec.data); });
        addPipeline(cases, spec.name, u"One linear symbol, 2-dot modules, caption below"_s,
                    u"^BY2,3,60^FO40,40^" + spec.command + u"N,60,Y,N^FD" + spec.data + u"^FS");
    }

    const QByteArray aztec = "QtZpl Aztec 123456789";
    addNative(cases, u"aztec"_s, u"Aztec dynamic size with 23 percent ECC"_s, aztec.size(),
              [aztec] { return Encoders::Aztec::encode(aztec); });
    addPipeline(cases, u"aztec"_s, u"Aztec at magnification three"_s,
                u"^FO40,40^BON,3,N,23,N^FDQtZpl Aztec 123456789^FS"_s);
    const auto gtin = u"0950110153000"_s;
    addNative(cases, u"databar"_s, u"GS1 DataBar omnidirectional modules"_s, gtin.size(),
              [gtin] { return Encoders::DataBar::encode(gtin); });
    addPipeline(cases, u"databar"_s, u"GS1 DataBar omnidirectional at magnification two"_s,
                u"^FO40,40^BRN,1,2,1,25^FD" + gtin + u"^FS");
    const QByteArray micro = "ABCDEFG";
    addNative(cases, u"micropdf417"_s, u"MicroPDF417 fixed two-column mode 7"_s, micro.size(),
              [micro] { return Encoders::MicroPdf417::encode(micro, 7); });
    addPipeline(cases, u"micropdf417"_s, u"MicroPDF417 at two-dot modules and three-dot row height"_s,
                u"^BY2^FO40,40^BFN,3,7^FDABCDEFG^FS"_s);

    const auto sscc = u"0012345678901234567"_s;
    addNative(cases, u"code128-u"_s, u"Code128 UCC case mode, generated Mod 10 and FNC1"_s, sscc.size(),
              [sscc] { return Encoders::code128(sscc, u'U'); });
    addPipeline(cases, u"code128-u"_s, u"Code128 UCC case mode without interpretation"_s,
                u"^BY2^FO40,40^BCN,60,N,N,N,U^FD" + sscc + u"^FS");
    const auto checkedDigits = u"12345678901"_s;
    addNative(cases, u"code128-mod10"_s, u"Code128 automatic subset and optional UCC Mod 10"_s, checkedDigits.size(),
              [checkedDigits] { return Encoders::code128(checkedDigits, u'A', true); });
    addPipeline(cases, u"code128-mod10"_s, u"Code128 optional UCC Mod 10 without interpretation"_s,
                u"^BY2^FO40,40^BCN,60,N,N,Y,A^FD" + checkedDigits + u"^FS");

    addNative(cases, u"datamatrix-rectangular"_s, u"Rectangular ECC200 12 by 26 modules"_s, 6,
              [] { return Encoders::dataMatrix("123456", false, 12, 26, true); });
    addPipeline(cases, u"datamatrix-rectangular"_s, u"Rectangular ECC200 at magnification three"_s,
                u"^FO40,40^BXN,3,200,26,12,6,~,2^FD123456^FS"_s);

    for (int mode = 3; mode <= 6; ++mode) {
        const auto name = u"maxicode-mode%1"_s.arg(mode);
        const QByteArray data = mode == 3 ? QByteArrayLiteral("456123ABCDEF[)>\x1e" "01\x1d" "96A\x1e\x04")
                                          : QByteArrayLiteral("HELLO WORLD 123456789");
        addNative(cases, name, u"MaxiCode primary/secondary encoding, RS and module placement"_s, data.size(),
                  [data, mode] { return Encoders::MaxiCode::encode(data, mode); });
        const auto zplData = mode == 3 ? u"456123ABCDEF[)>_1E01_1D96A_1E_04"_s : QString::fromLatin1(data);
        addPipeline(cases, name, u"MaxiCode mode %1, default orientation and carrier geometry"_s.arg(mode),
                    u"^FO40,40^BD%1,1,1^FH_^FD"_s.arg(mode) + zplData + u"^FS");
    }

    addNative(cases, u"pdf417-truncated"_s, u"Truncated PDF417 level 2, two columns, eight rows"_s, 6,
              [] { return Encoders::Pdf417::encode("ABCDEF", 2, 2, 8, true); });
    addPipeline(cases, u"pdf417-truncated"_s, u"Truncated PDF417 with two-dot modules and three-dot row height"_s,
                u"^BY2^FO40,40^B7N,3,2,2,8,Y^FDABCDEF^FS"_s);

    addPipeline(cases, u"fp-spacing"_s, u"Forward, reverse and vertical text with explicit glyph spacing"_s,
                u"^FO40,40^ADN,30,18^FPH,3^FDABC123^FS"
                u"^FO400,80^ADN,30,18^FPR,3^FDABC123^FS"
                u"^FO600,40^ADN,30,18^FPV,3^FDABC123^FS"_s);
    addPipeline(cases, u"fo-ft-transforms"_s, u"FO/FT anchors through label top, mirror, reverse and home offsets"_s,
                u"^LH20,15^LT12^PMY^FO20,20^GB500,180,180^FS^LRY"
                u"^FO40,40^ADN,30,18^FDORIGIN ABC^FS"
                u"^FT40,110^ADN,30,18^FDBASELINE XYZ^FS"
                u"^FO40,140^GB200,20,3^FS^LRN^FO550,40^BY2^BFN,3,7^FDABCDEFG^FS"_s);
}
} // namespace Bench
