#include <QtZpl/qtzpl.hpp>

#include <QtCore/QLatin1StringView>
#include <algorithm>
#include <optional>

namespace QtZpl {
using namespace Qt::StringLiterals;

int intValue(const QList<QString>& p, qsizetype index, int fallback = 0) {
  if (index >= p.size() || p[index].trimmed().isEmpty()) return fallback;
  bool ok = false;
  const auto value = p[index].trimmed().toInt(&ok);
  return ok ? value : fallback;
}

double doubleValue(const QList<QString>& p, qsizetype index, double fallback) {
  if (index >= p.size() || p[index].trimmed().isEmpty()) return fallback;
  bool ok = false;
  const auto value = p[index].trimmed().toDouble(&ok);
  return ok ? value : fallback;
}

QChar charValue(const QList<QString>& p, qsizetype index, QChar fallback) {
  return index < p.size() && !p[index].isEmpty() ? p[index].front().toUpper() : fallback;
}

Orientation orientationValue(const QList<QString>& p, qsizetype index = 0) {
  switch (charValue(p, index, u'N').unicode()) {
    case 'R': return Orientation::Rotated90;
    case 'I': return Orientation::Inverted;
    case 'B': return Orientation::BottomUp;
    default: return Orientation::Normal;
  }
}

Justification justificationValue(const QList<QString>& p, qsizetype index) {
  switch (charValue(p, index, u'L').unicode()) {
    case 'R': return Justification::Right;
    case 'C': return Justification::Center;
    case 'J': return Justification::Justified;
    default: return Justification::Left;
  }
}

LineColor colorValue(const QList<QString>& p, qsizetype index) {
  return charValue(p, index, u'B') == u'W' ? LineColor::White : LineColor::Black;
}

class Parser final {
public:
  Parser(QStringView input, ParseOptions options) : input_(input), options_(options) {}

  std::expected<Document, ParseError> run() {
    while (position_ < input_.size()) {
      const auto marker = input_[position_];
      if (marker != caret_ && marker != tilde_) { ++position_; continue; }
      const auto offset = position_++;
      if (position_ >= input_.size()) break;
      auto opcode = readOpcode();
      if (opcode.isEmpty()) continue;
      auto paramsText = readParameters(opcode);
      if (!consume(marker, opcode, paramsText, offset)) {
        return std::unexpected(ParseError{u"malformed-command"_s,
          u"Cannot parse command "_s + marker + opcode, offset});
      }
    }

    if (current_) {
      diagnostics_.append({Severity::Warning, u"unterminated-format"_s,
        u"The final ^XA block has no ^XZ; it was preserved."_s, input_.size(), u"^XZ"_s});
      finishLabel();
    }
    document_.diagnostics_ = diagnostics_;
    return document_;
  }

private:
  QString readOpcode() {
    if (position_ >= input_.size()) return {};
    const auto first = input_[position_++].toUpper();
    if (first == u'A' && position_ < input_.size()) {
      const auto second = input_[position_];
      if (second == u'@' || second.isLetterOrNumber()) { ++position_; return QString{first} + second.toUpper(); }
    }
    if (position_ < input_.size() && input_[position_].isLetterOrNumber()) {
      return QString{first} + input_[position_++].toUpper();
    }
    return QString{first};
  }

  QString readParameters(const QString& opcode) {
    if (opcode == u"FD" || opcode == u"FV") {
      const auto start = position_;
      while (position_ < input_.size()) {
        if (input_[position_] == caret_ && input_.mid(position_ + 1, 2).compare(u"FS", Qt::CaseInsensitive) == 0) break;
        ++position_;
      }
      return input_.mid(start, position_ - start).toString();
    }
    if (opcode == u"GF") {
      const auto start = position_;
      qsizetype dataStart = -1;
      for (int comma = 0; comma < 4; ++comma) {
        dataStart = input_.indexOf(u',', dataStart < 0 ? start : dataStart + 1);
        if (dataStart < 0) break;
      }
      if (dataStart >= 0) {
        const auto header = input_.mid(start, dataStart - start).toString();
        const auto parameters = split(header);
        const auto compression = charValue(parameters, 0, u'A');
        if (compression == u'B' || compression == u'C') {
          const int dataBytes = std::max(0, intValue(parameters, 2));
          const auto available = std::min<qsizetype>(dataBytes, input_.size() - dataStart - 1);
          position_ = dataStart + 1 + available;
          return header + u',' + input_.mid(dataStart + 1, available).toString();
        }
      }
    }
    const auto start = position_;
    while (position_ < input_.size() && input_[position_] != caret_ && input_[position_] != tilde_) ++position_;
    return input_.mid(start, position_ - start).toString().remove(u'\r').remove(u'\n');
  }

  QList<QString> split(const QString& text) const { return text.split(u',', Qt::KeepEmptyParts); }

  void add(CommandPayload payload, qsizetype offset, QChar prefix, const QString& opcode, const QString& params) {
    if (!current_) return;
    current_->commands_.append(Command{std::move(payload), offset, QString{prefix} + opcode + params});
  }

  QString decodeHex(QString value) {
    if (hexIndicator_.isNull()) return value;
    QString decoded;
    decoded.reserve(value.size());
    for (qsizetype i = 0; i < value.size(); ++i) {
      if (value[i] == hexIndicator_ && i + 2 < value.size()) {
        bool ok = false;
        const auto byte = value.mid(i + 1, 2).toUInt(&ok, 16);
        if (ok) { decoded.append(QChar::fromLatin1(static_cast<char>(byte))); i += 2; continue; }
      }
      decoded.append(value[i]);
    }
    hexIndicator_ = {};
    return decoded;
  }

  void finishLabel() {
    document_.labels_.append(std::move(*current_));
    current_.reset();
    inFormat_ = false;
    hexIndicator_ = {};
  }

  bool consume(QChar prefix, const QString& op, const QString& raw, qsizetype offset) {
    if (prefix == tilde_) return unknown(prefix, op, raw, offset);
    if (op == u"XA") {
      if (current_) {
        diagnostics_.append({Severity::Warning, u"nested-format"_s, u"A new ^XA closed the previous format."_s, offset, u"^XA"_s});
        finishLabel();
      }
      current_.emplace(); inFormat_ = true;
      add(FormatStart{}, offset, prefix, op, raw); return true;
    }
    if (op == u"XZ") {
      if (!current_) return unknown(prefix, op, raw, offset);
      add(FormatEnd{}, offset, prefix, op, raw); finishLabel(); return true;
    }
    if (!inFormat_ || !current_) return true;

    const auto p = split(raw);
    if (op == u"FO") add(FieldOrigin{intValue(p,0), intValue(p,1), justificationValue(p,2)}, offset,prefix,op,raw);
    else if (op == u"FT") add(FieldTypeset{intValue(p,0), intValue(p,1), justificationValue(p,2)}, offset,prefix,op,raw);
    else if (op == u"FD" || op == u"FV") add(FieldData{decodeHex(raw)}, offset,prefix,op,raw);
    else if (op == u"FS") add(FieldSeparator{}, offset,prefix,op,raw);
    else if (op == u"FR") add(FieldReverse{}, offset,prefix,op,raw);
    else if (op == u"FW" || op == u"FP") add(FieldDirection{orientationValue(p)}, offset,prefix,op,raw);
    else if (op == u"FH") hexIndicator_ = raw.isEmpty() ? u'_' : raw.front();
    else if (op == u"A@") parseDownloadedFont(p,offset,raw);
    else if (op.size() == 2 && op.front() == u'A') parseFont(op, p, offset, raw);
    else if (op == u"CF") add(ChangeFont{charValue(p,0,u'0'), intValue(p,1,30), intValue(p,2,0)}, offset,prefix,op,raw);
    else if (op == u"FB") add(FieldBlock{intValue(p,0), intValue(p,1,1), intValue(p,2), justificationValue(p,3), intValue(p,4)}, offset,prefix,op,raw);
    else if (op == u"F8") add(FieldEncoding{raw},offset,prefix,op,raw);
    else if (op == u"CI") add(CharacterSet{intValue(p,0)}, offset,prefix,op,raw);
    else if (op == u"BY") add(BarcodeDefault{intValue(p,0,2), doubleValue(p,1,3.0), intValue(p,2,10)}, offset,prefix,op,raw);
    else if (op == u"PW") { current_->width_ = intValue(p,0); add(PrintWidth{current_->width_}, offset,prefix,op,raw); }
    else if (op == u"LL") { current_->height_ = intValue(p,0); add(LabelLength{current_->height_}, offset,prefix,op,raw); }
    else if (op == u"LH") { current_->homeX_=intValue(p,0); current_->homeY_=intValue(p,1); add(LabelHome{current_->homeX_,current_->homeY_},offset,prefix,op,raw); }
    else if (op == u"LS") add(LabelShift{intValue(p,0)},offset,prefix,op,raw);
    else if (op == u"MM") add(PrintMode{charValue(p,0,u'T'),charValue(p,1,u'N')==u'Y'},offset,prefix,op,raw);
    else if (op == u"PO") add(PrintOrientation{orientationValue(p)},offset,prefix,op,raw);
    else if (op == u"PR") add(PrintRate{raw},offset,prefix,op,raw);
    else if (op == u"MD") add(MediaDarkness{intValue(p,0)},offset,prefix,op,raw);
    else if (op == u"PQ") add(PrintQuantity{std::max(1,intValue(p,0,1)),intValue(p,1),intValue(p,2),charValue(p,3,u'N')==u'Y'},offset,prefix,op,raw);
    else if (op == u"GB") add(GraphicBox{intValue(p,0),intValue(p,1),std::max(1,intValue(p,2,1)),colorValue(p,3),intValue(p,4)},offset,prefix,op,raw);
    else if (op == u"GC") add(GraphicCircle{intValue(p,0),std::max(1,intValue(p,1,1)),colorValue(p,2)},offset,prefix,op,raw);
    else if (op == u"GD") add(GraphicDiagonal{intValue(p,0),intValue(p,1),std::max(1,intValue(p,2,1)),colorValue(p,3),charValue(p,4,u'R')},offset,prefix,op,raw);
    else if (op == u"GE") add(GraphicEllipse{intValue(p,0),intValue(p,1),std::max(1,intValue(p,2,1)),colorValue(p,3)},offset,prefix,op,raw);
    else if (op == u"GF") parseGraphicField(p, offset, raw);
    else if (op == u"FX") add(Comment{raw},offset,prefix,op,raw);
    else if (isBarcode(op)) parseBarcode(op, p, offset, raw);
    else return unknown(prefix, op, raw, offset);
    return true;
  }

  void parseFont(const QString& op, const QList<QString>& p, qsizetype offset, const QString& raw) {
    const QChar font = op[1];
    int defaultHeight = 30, defaultWidth = 0;
    if (font == u'A') { defaultHeight=9; defaultWidth=5; }
    else if (font == u'B') { defaultHeight=11; defaultWidth=7; }
    else if (font == u'C' || font == u'D') { defaultHeight=18; defaultWidth=20; }
    else if (font == u'E') { defaultHeight=28; defaultWidth=15; }
    else if (font == u'F') { defaultHeight=26; defaultWidth=13; }
    else if (font == u'G') { defaultHeight=60; defaultWidth=40; }
    else if (font == u'H') { defaultHeight=21; defaultWidth=13; }
    add(ScalableFont{font,orientationValue(p),intValue(p,1,defaultHeight),intValue(p,2,defaultWidth)},offset,caret_,op,raw);
  }

  void parseDownloadedFont(const QList<QString>& p,qsizetype offset,const QString& raw) {
    // External printer fonts are not available to the local deterministic renderer.
    // Preserve ^A@ as a known command and use the embedded scalable Font 0 metrics.
    add(ScalableFont{u'0',orientationValue(p),intValue(p,1,30),intValue(p,2,0)},offset,caret_,u"A@"_s,raw);
  }

  void parseGraphicField(const QList<QString>& p, qsizetype offset, const QString& raw) {
    const auto firstComma = raw.indexOf(u',');
    const auto dataStart = [&] { qsizetype pos=-1; for(int i=0;i<4;++i) pos=raw.indexOf(u',',pos+1); return pos; }();
    GraphicField gf{charValue(p,0,u'A'),intValue(p,2),intValue(p,1),intValue(p,3),{}};
    if (dataStart >= 0) {
      const auto data=raw.mid(dataStart+1);
      gf.data = gf.compression == u'A' ? data.trimmed().toLatin1() : data.toLatin1();
    }
    Q_UNUSED(firstComma);
    add(std::move(gf),offset,caret_,u"GF"_s,raw);
  }

  bool isBarcode(const QString& op) const {
    static const QList<QString> codes{u"BC"_s,u"B3"_s,u"BE"_s,u"BU"_s,u"BI"_s,u"B2"_s,u"BK"_s,u"BQ"_s,u"BX"_s,u"B7"_s,u"BD"_s,u"BO"_s};
    return codes.contains(op);
  }

  void parseBarcode(const QString& op, const QList<QString>& p, qsizetype offset, const QString& raw) {
    Barcode barcode{op,orientationValue(p),p,{}};
    // ZPL barcode data belongs to the immediately following ^FD. Keep the command
    // in the model and let the renderer consume that FieldData deterministically.
    add(std::move(barcode),offset,caret_,op,raw);
  }

  bool unknown(QChar prefix, const QString& op, const QString& raw, qsizetype offset) {
    diagnostics_.append({Severity::Warning,u"unsupported-command"_s,u"Unsupported command was skipped."_s,offset,QString{prefix}+op});
    if (current_ && options_.preserveUnknownCommands) add(UnknownCommand{prefix,op,raw,offset},offset,prefix,op,raw);
    return true;
  }

  QStringView input_;
  ParseOptions options_;
  Document document_;
  QList<Diagnostic> diagnostics_;
  std::optional<Label> current_;
  qsizetype position_ = 0;
  bool inFormat_ = false;
  QChar caret_ = u'^';
  QChar tilde_ = u'~';
  QChar hexIndicator_;
};

std::expected<Document, ParseError> parse(QStringView zpl, const ParseOptions& options) {
  return Parser{zpl, options}.run();
}

} // namespace QtZpl
