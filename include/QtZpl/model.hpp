#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QList>
#include <QtCore/QString>
#include <QtCore/QVariant>
#include <QtCore/QtGlobal>
#include <optional>
#include <variant>

#include <QtZpl/export.hpp>

namespace QtZpl {

enum class Severity { Warning, Error };
enum class Orientation : char { Normal = 'N', Rotated90 = 'R', Inverted = 'I', BottomUp = 'B' };
enum class Justification : char { Left = 'L', Right = 'R', Center = 'C', Justified = 'J' };
enum class LineColor : char { Black = 'B', White = 'W' };

struct Diagnostic {
  Severity severity = Severity::Warning;
  QString code;
  QString message;
  qsizetype offset = -1;
  QString command;
};

struct FormatStart {};
struct FormatEnd {};
struct FieldSeparator {};
struct FieldReverse {};
struct FieldOrigin { int x = 0; int y = 0; Justification justification = Justification::Left; };
struct FieldTypeset { int x = 0; int y = 0; Justification justification = Justification::Left; };
struct FieldData { QString data; };
struct FieldDirection { Orientation orientation = Orientation::Normal; };
struct FieldBlock { int width = 0; int maxLines = 1; int lineSpacing = 0; Justification justification = Justification::Left; int hangingIndent = 0; };
struct ScalableFont { QChar font = u'0'; Orientation orientation = Orientation::Normal; int height = 30; int width = 0; };
struct ChangeFont { QChar font = u'0'; int height = 30; int width = 0; };
struct CharacterSet { int id = 0; };
struct BarcodeDefault { int moduleWidth = 2; double wideToNarrowRatio = 3.0; int height = 10; };
struct PrintWidth { int dots = 0; };
struct LabelLength { int dots = 0; };
struct LabelHome { int x = 0; int y = 0; };
struct PrintOrientation { Orientation orientation = Orientation::Normal; };
struct PrintRate { QString parameters; };
struct MediaDarkness { int darkness = 0; };
struct PrintQuantity { int quantity = 1; int pauseAndCut = 0; int replicates = 0; bool overridePauseCount = false; };
struct GraphicBox { int width = 0; int height = 0; int thickness = 1; LineColor color = LineColor::Black; int cornerRounding = 0; };
struct GraphicCircle { int diameter = 0; int thickness = 1; LineColor color = LineColor::Black; };
struct GraphicDiagonal { int width = 0; int height = 0; int thickness = 1; LineColor color = LineColor::Black; QChar orientation = u'R'; };
struct GraphicEllipse { int width = 0; int height = 0; int thickness = 1; LineColor color = LineColor::Black; };
struct GraphicField { QChar compression = u'A'; int totalBytes = 0; int bytesUsed = 0; int bytesPerRow = 0; QByteArray data; };
struct Barcode {
  QString symbology;
  Orientation orientation = Orientation::Normal;
  QList<QString> parameters;
  QString data;
};
struct Comment { QString text; };
struct UnknownCommand { QChar prefix = u'^'; QString opcode; QString parameters; qsizetype offset = -1; };

using CommandPayload = std::variant<
    FormatStart, FormatEnd, FieldSeparator, FieldReverse, FieldOrigin, FieldTypeset,
    FieldData, FieldDirection, FieldBlock, ScalableFont, ChangeFont, CharacterSet,
    BarcodeDefault, PrintWidth, LabelLength, LabelHome, PrintOrientation, PrintRate,
    MediaDarkness, PrintQuantity, GraphicBox,
    GraphicCircle, GraphicDiagonal, GraphicEllipse, GraphicField, Barcode, Comment,
    UnknownCommand>;

struct Command {
  CommandPayload payload;
  qsizetype offset = -1;
  QString source;
};

class QTZPL_EXPORT Label final {
public:
  [[nodiscard]] int width() const noexcept { return width_; }
  [[nodiscard]] int height() const noexcept { return height_; }
  [[nodiscard]] int homeX() const noexcept { return homeX_; }
  [[nodiscard]] int homeY() const noexcept { return homeY_; }
  [[nodiscard]] const QList<Command>& commands() const noexcept { return commands_; }

private:
  friend class Parser;
  int width_ = 0;
  int height_ = 0;
  int homeX_ = 0;
  int homeY_ = 0;
  QList<Command> commands_;
};

class QTZPL_EXPORT Document final {
public:
  [[nodiscard]] const QList<Label>& labels() const noexcept { return labels_; }
  [[nodiscard]] const QList<Diagnostic>& diagnostics() const noexcept { return diagnostics_; }

private:
  friend class Parser;
  QList<Label> labels_;
  QList<Diagnostic> diagnostics_;
};

} // namespace QtZpl
