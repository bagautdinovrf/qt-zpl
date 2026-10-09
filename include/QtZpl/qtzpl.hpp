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

// Effective interpretation state before final label rotation/mirroring.
struct FieldSettings {
  QPoint position;
  bool baseline = false;
  Justification justification = Justification::Left;
  ScalableFont font{u'A', Orientation::Normal, 9, 5};
  ScalableFont defaultFont{u'A', Orientation::Normal, 9, 5};
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
