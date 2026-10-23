#pragma once

#include <expected>
#include <stop_token>
#include <QtCore/QPoint>
#include <QtCore/QRect>
#include <QtCore/QSize>
#include <QtCore/QStringView>
#include <QtGui/QColor>
#include <QtGui/QImage>
#include <QtGui/QTransform>

#include "model.hpp"

namespace QtZpl {

struct ParseOptions {
  bool preserveUnknownCommands = true;
  qsizetype maxSourceLength = 16 * 1024 * 1024;
  qsizetype maxCommands = 1000000;
  qsizetype maxLabels = 10000;
  qsizetype maxDiagnostics = 10000;
  std::stop_token stopToken;
};
struct RenderOptions {
  int dpi = 203;
  QColor foreground = Qt::black;
  QColor background = Qt::white;
  int width = 0;
  int height = 0;
  bool ignoreLabelHome = false;
  bool collectFieldGeometry = false;
  qint64 maxTotalPixels = 256LL * 1024 * 1024;
  qsizetype maxGraphicBytes = 64 * 1024 * 1024;
  qsizetype maxDiagnostics = 10000;
  std::stop_token stopToken;
};

struct ParseError { QString code; QString message; qsizetype offset = -1; };
struct RenderError { QString code; QString message; int labelIndex = -1; };
struct Error {
  enum class Stage { Parse, Render } stage = Stage::Parse;
  QString code;
  QString message;
  qsizetype offset = -1;
  int labelIndex = -1;
};
enum class FieldKind { Text, Barcode, Box, Circle, Ellipse, Diagonal, Graphic, Unknown };
enum class FieldStatus { Drawn, Empty, Clipped, Unsupported, Error };

// Barcode commands as the renderer interprets them (0.4.0). One resolver
// supplies both rendering and analysis; the raw Barcode keeps the spelling.
enum class Symbology {
  Unknown, Code128, Code39, Code93, Codabar, Ean13, Ean8, UpcA, UpcE,
  Interleaved2of5, Industrial2of5, DataBar, QrCode, DataMatrix, Pdf417,
  MicroPdf417, Aztec, MaxiCode
};
struct Code128Options { QChar mode = u'N'; bool uccCheckDigit = false; };
// e parameter: Code 39 Mod 43, Codabar (fixed N), Code 93 and Interleaved
// 2 of 5 check digits; UPC-A/UPC-E print their check digit unless e is N.
// Codabar only: start and stop characters from ^BK k and l (upper-case A-D;
// A when omitted or invalid). Field data supplies no start/stop characters;
// rendering drops field characters outside 0-9 - $ : / . + with a warning
// and draws no symbol when none is left.
struct LinearOptions { bool checkDigit = false; QChar start; QChar stop; };
struct QrCodeOptions { int model = 2; };
struct DataMatrixOptions {
  int quality = 200; // Empty is ECC 200, the only quality QtZpl renders.
  int columns = 0;
  int rows = 0;
  int format = 0;
  QChar escape = u'~';
  bool rectangular = false;
  bool gs1 = false; // Format 1 selects GS1 DataMatrix in QtZpl.
};
struct Pdf417Options { int security = 0; int columns = 0; int rows = 0; bool truncated = false; };
struct MicroPdf417Options { int mode = 0; };
struct DataBarOptions { int type = 1; int separatorHeight = 1; int segments = 0; };
// size: 1-99 error correction percent, 101-104 compact and 201-232 full-range
// layers, 300 rune. Omitted, 0 and other values are ignored like Labelary does
// and resolve to the default 23.
struct AztecOptions { int size = 23; bool eci = false; bool readerInitialization = false; int structuredAppend = 1; };
struct MaxiCodeOptions { int mode = 2; int position = 1; int total = 1; };
// New alternatives are appended so existing indices remain stable.
using BarcodeOptions = std::variant<std::monostate, Code128Options, LinearOptions, QrCodeOptions,
  DataMatrixOptions, Pdf417Options, MicroPdf417Options, DataBarOptions, AztecOptions, MaxiCodeOptions>;

struct ResolvedBarcode {
  Symbology symbology = Symbology::Unknown;
  // Effective rotation. When the parameter is omitted, the ^FW in effect at
  // the barcode command supplies it; a later ^FW in the field does not. QR
  // modules never rotate; only its ^FO margin follows this orientation.
  Orientation orientation = Orientation::Normal;
  bool orientationOmitted = false;
  // Narrow bar or module width in dots after ^BY, the ^BQ side effect,
  // symbol parameters and clamping. 0 for MaxiCode's fixed geometry.
  int moduleWidth = 0;
  // ^BY ratio as the symbology applies it (Codabar rounds to 2 or 3).
  double wideToNarrowRatio = 3.0;
  // Bar height for linear symbols and GS1 DataBar, row height for PDF417
  // and MicroPDF417, 0 for other matrix symbols. Defaults are applied.
  int height = 0;
  // Requested human-readable line. Code 39 and Codabar do not draw it yet
  // and report barcode-interpretation-pending when rendering.
  bool interpretation = false;
  bool interpretationAbove = false;
  BarcodeOptions options;
};

// Effective interpretation state before final label rotation/mirroring.
struct FieldSettings {
  QPoint position;
  bool baseline = false;
  Justification justification = Justification::Left;
  // font.orientationOmitted: ^FW supplies the text rotation, because the
  // parameter is omitted or a later ^FW replaced the explicit one.
  ScalableFont font{u'A', Orientation::Normal, 9, 5, true};
  ScalableFont defaultFont{u'A', Orientation::Normal, 9, 5, true};
  std::optional<ScalableFont> barcodeInterpretationFont;
  Orientation fieldDirection = Orientation::Normal;
  BarcodeDefault barcodeDefaults;
  std::optional<Barcode> barcode;
  std::optional<FieldBlock> block;
  FieldParameter parameter;
  int characterSet = 0;
  QChar hexIndicator;
  QPoint labelHome;
  int labelShift = 0;
  int labelTop = 0;
  bool reverse = false;
  Orientation printOrientation = Orientation::Normal;
  bool mirror = false;
  // Appended in 0.4.0. Set for a barcode field whose parameters resolve;
  // empty when rendering would report a parameter error for the command.
  std::optional<ResolvedBarcode> resolvedBarcode;
};

struct FieldInfo {
  // Source offset of the drawing command, unique only in this parsed snapshot.
  qsizetype id = -1;
  int labelIndex = -1;
  qsizetype commandIndex = -1;
  SourceSpan sourceSpan; // Containing field segment, including its ^FS if present.
  SourceSpan payloadSpan; // Drawing command, including opcode.
  FieldKind kind = FieldKind::Unknown;
  FieldSettings settings;
  bool hasUnknownCommands = false;
};

struct FieldGeometry {
  FieldInfo field;
  // All bounds are integer dot enclosures in the final output coordinate system.
  QRect logicalBounds; // Layout box, before clipping; not an ink measurement.
  QRect paintBounds; // Own paint operations, including backgrounds and white ink.
  QRect clippedBounds; // Intersection of paintBounds and the output canvas.
  QPoint anchor;
  bool baseline = false;
  QTransform labelTransform; // Pre-label coordinates -> final output coordinates.
  FieldStatus status = FieldStatus::Empty;
};

struct AnalysisResult { QList<FieldInfo> fields; QList<Diagnostic> diagnostics; };
struct RenderResult {
  QList<QImage> labels;
  QList<Diagnostic> diagnostics;
  QList<FieldGeometry> fields;
};
struct LabelRenderResult {
  QImage image;
  int labelIndex = -1;
  QList<Diagnostic> diagnostics;
  QList<FieldGeometry> fields;
};

struct GraphicDecodeOptions {
  qsizetype maxDecodedBytes = 64 * 1024 * 1024;
  std::stop_token stopToken;
  qsizetype maxEncodedBytes = 128 * 1024 * 1024;
};
struct GraphicError { QString code; QString message; };
struct DecodedGraphic {
  QSize size;
  int bytesPerRow = 0;
  // MSB first, row-major packed bits. Incomplete binary final rows are zero padded.
  QByteArray bytes;
};

QTZPL_EXPORT std::expected<Document, ParseError> parse(QStringView zpl, const ParseOptions& options = {});
QTZPL_EXPORT std::expected<RenderResult, RenderError> render(const Document& document, const RenderOptions& options = {});
QTZPL_EXPORT std::expected<LabelRenderResult, RenderError> renderLabel(const Document& document, int labelIndex, const RenderOptions& options = {});
QTZPL_EXPORT std::expected<AnalysisResult, RenderError> analyze(const Document& document, const RenderOptions& options = {});
QTZPL_EXPORT std::expected<AnalysisResult, RenderError> analyzeLabel(const Document& document, int labelIndex, const RenderOptions& options = {});
QTZPL_EXPORT std::expected<DecodedGraphic, GraphicError> decodeGraphic(const GraphicField& graphic, const GraphicDecodeOptions& options = {});
QTZPL_EXPORT std::expected<RenderResult, Error> render(QStringView zpl, const ParseOptions& parseOptions = {}, const RenderOptions& renderOptions = {});

} // namespace QtZpl
