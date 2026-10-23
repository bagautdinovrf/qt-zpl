#pragma once

// Internal interface between the renderer's translation units: one ZPL
// interpreter (renderer.cpp) with fonts, text, graphics and barcode drawing in
// their own files. Not installed and not exported; the shared library hides
// these symbols (CXX_VISIBILITY_PRESET hidden).

#include <QtZpl/qtzpl.hpp>
#include "font0_advance_data.hpp"
#include "font0_face.hpp"
#include "printer_font_raster.hpp"

#include <QtCore/QtMath>
#include <QtGui/QFont>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <algorithm>
#include <expected>
#include <limits>
#include <optional>
#include <utility>

namespace QtZpl::Rendering {

// A command's position is its sourceSpan start; Diagnostic::command keeps the
// normalized spelling, which only the deprecated Command::source carries.
inline qsizetype commandOffset(const Command& command) { return command.sourceSpan.start; }
inline QString normalizedCommand(const Command& command) {
  QT_WARNING_PUSH
  QT_WARNING_DISABLE_DEPRECATED
  return command.source;
  QT_WARNING_POP
}

inline QPoint printerPosition(qint64 x,qint64 y) {
  // Off-canvas coordinates remain off canvas without signed overflow during
  // later baseline, bearing, or orientation arithmetic.
  constexpr qint64 limit=std::numeric_limits<int>::max()/4;
  return {int(std::clamp(x,-limit,limit)),int(std::clamp(y,-limit,limit))};
}

struct State {
  // Optional per-field observer. It records the field's own operations before
  // composition, so white/reverse fields and later overpainting retain bounds.
  struct Observation { QRect logicalBounds; QRect paintBounds; };
  Observation* observation = nullptr;
  bool rasterize = true;
  QPoint position;
  QPoint nextTypeset;
  int labelShift = 0;
  bool baseline = false;
  bool reverse = false;
  bool fieldReverse = false;
  bool labelReverse = false;
  FieldParameter parameter;
  Justification justification = Justification::Left;
  Justification defaultJustification = Justification::Left;
  Orientation fieldDirection = Orientation::Normal;
  // ^FW in effect at the pending barcode command. A later ^FW in the same
  // field does not rotate a barcode, unlike a following text field's font.
  Orientation barcodeDirection = Orientation::Normal;
  ScalableFont font{u'A',Orientation::Normal,9,5,true};
  ScalableFont defaultFont{u'A',Orientation::Normal,9,5,true};
  std::optional<ScalableFont> barcodeInterpretationFont;
  std::optional<FieldBlock> block;
  BarcodeDefault barcodeDefaults;
  std::optional<Barcode> pendingBarcode;
  qsizetype barcodeOffset = -1;
  QString barcodeSource;
  Orientation printOrientation = Orientation::Normal;
  int characterSet = 0;
  QChar hexIndicator;
};

inline void observeLogical(const State& state,const QRect& bounds) {
  if(state.observation&&!bounds.isEmpty())state.observation->logicalBounds|=bounds;
}

inline void observePaint(const State& state,const QRect& bounds) {
  if(state.observation&&!bounds.isEmpty())state.observation->paintBounds|=bounds;
}

inline QColor commandColor(LineColor color, const RenderOptions& options) {
  return color == LineColor::White ? options.background : options.foreground;
}

// Fonts and metrics (render_fonts.cpp).
QString fontAFamily();
QFont printerFont(const QString& family,int pixelHeight,int stretch=100);
QFont zebraFont(int pixelHeight, int stretch=75);
QFont builtInFont(QChar name,int pixelHeight,int requestedWidth=0);
QSize resolvedFontSize(const ScalableFont& font);
QImage rotateImage(const QImage& source, Orientation orientation);
QRect alphaBounds(const QImage& image);
void observeRaster(const State& state,QPoint position,const QImage& raster,bool logical=true);

// Keep one lazily-created native face per render call, including all labels
// in that call, without sharing mutable font data between concurrent renders.
class RenderFonts {
public:
  Font0Face& font0() {
    if(!font0_)font0_.emplace(font0Bytes());
    if(!font0_->isValid())font0Error_=font0_->errorString();
    return *font0_;
  }
  QPainterPath font0Glyph(quint32 glyph) {
    const auto result=font0().glyph(glyph);
    if(result)return result->path;
    font0Error_=result.error();return {};
  }
  qreal font0Advance(quint32 glyph,int width) {
    if(glyph<Font0AdvanceData::advances2048.size()) {
      const auto advance=Font0AdvanceData::advances2048[glyph];
      if(advance>=0)return qreal(advance)*width/Font0AdvanceData::unitsPerEm;
    }
    const auto result=font0().advance(glyph);
    if(result)return *result*width/1000;
    font0Error_=result.error();return 0;
  }
  QString takeFont0Error() {return std::exchange(font0Error_,{});}
  // Called for every Font A glyph: keep the loaded case inline.
  PrinterFontRaster& fontARaster() {
    if(!fontARaster_)loadFontARaster();
    return *fontARaster_;
  }
private:
  void loadFontARaster();
  static const QByteArray& font0Bytes();
  std::optional<Font0Face> font0_;
  std::optional<PrinterFontRaster> fontARaster_;
  QString font0Error_;
};

// Rectangular Font 0 hyphen (U+002D) measured from Labelary references.
QRectF font0HyphenBounds(qreal penX,qreal penY,int width,int height,Orientation orientation);

// Use the font's design grid rather than QFont::setStretch. On Windows the
// latter quantizes widths through GDI's integer average-character width, while
// other Qt backends use a different scale. Font 0 has a 1000-unit em and a
// 750-unit cap height; independent ZPL height/width scale that same design grid.
class Font0Metrics {
public:
  Font0Metrics(int height,int width,RenderFonts& fontContext,FieldParameter parameter={},
               Orientation orientation=Orientation::Normal)
    : font(fontContext.font0()),fonts(fontContext),
      fontWidth(std::max(10,width>0?width:height)),scaleX(qreal(fontWidth)/1000),
      scaleY(static_cast<qreal>(std::max(10,height))/1000.0),fontHeight(std::max(10,height)),
      parameter(parameter),orientation(orientation) {}

  [[nodiscard]] int ascent() const {return qFloor(font.capHeight()*scaleY);}
  [[nodiscard]] int height() const {return std::max(1,fontHeight);}
  [[nodiscard]] int cellWidth() const {
    return qRound(qreal(Font0AdvanceData::nominalCellAdvance2048)*fontWidth/Font0AdvanceData::unitsPerEm);
  }
  [[nodiscard]] qreal designAdvance(const QString& text) const {
    const auto glyphs=font.glyphIndexesForString(text);
    qreal width=0;for(const auto glyph:glyphs)width+=fonts.font0Advance(glyph,fontWidth);
    return width+std::max<qsizetype>(0,glyphs.size()-1)*parameter.spacing;
  }
  [[nodiscard]] int horizontalAdvance(const QString& text) const {return qRound(designAdvance(text));}
  [[nodiscard]] QPainterPath outline(const QString& text) const {
    const auto glyphs=font.glyphIndexesForString(text);
    QPainterPath result;qreal x=parameter.direction==u'R'?cellWidth():0,y=0;
    for(qsizetype i=0;i<glyphs.size();++i){
      const qreal advance=fonts.font0Advance(glyphs[i],fontWidth)+parameter.spacing;
      // Reverse fields start at the nominal M-cell edge. Each character's
      // own width is subtracted before painting, including proportional text.
      if(parameter.direction==u'R')x-=advance;
      if(glyphs[i]==16)result.addRect(font0HyphenBounds(x,y,fontWidth,fontHeight,orientation));
      else{
        QTransform transform;transform.translate(x,y);transform.scale(scaleX,scaleY);
        result.addPath(transform.map(fonts.font0Glyph(glyphs[i])));
      }
      if(parameter.direction==u'V')y+=fontHeight;
      else if(parameter.direction!=u'R')x+=advance;
    }
    return result;
  }
  void draw(QPainter& painter,int x,int baseline,const QString& text,const QColor& color) const {
    const auto path=outline(text).translated(x,baseline);
    painter.fillPath(path,color);
  }
private:
  const Font0Face& font;
  RenderFonts& fonts;
  int fontWidth;
  qreal scaleX;
  qreal scaleY;
  int fontHeight;
  FieldParameter parameter;
  Orientation orientation;
};

// Text and its anchors (render_text.cpp).
QPoint rotatedEdge(QPoint anchor,QSize source,Orientation orientation);
void paintPrinterGlyph(QPainter& painter,QPoint baseline,const PrinterGlyph& glyph,const QColor& ink);
void paintPrinterFontAGlyph(QPainter& painter,QPoint baseline,QChar character,int height,int width,
                           const QColor& ink,RenderFonts& fonts,Orientation orientation=Orientation::Normal);
void drawText(QImage& image,State& state,const QString& text,const RenderOptions& options,RenderFonts& fonts,
              QList<Diagnostic>& diagnostics,qsizetype offset);

// Printer graphics (render_graphics.cpp).
void drawGraphicField(QImage& image,const QPoint& pos,const DecodedGraphic& decoded,const RenderOptions& options,
                      const State& state);

// Barcode parameters and symbols (render_barcodes.cpp). resolveBarcode is the
// only interpretation of barcode parameters, shared by rendering and analysis.
int qrMagnification(const Barcode& barcode);
std::expected<ResolvedBarcode,Diagnostic> resolveBarcode(const Barcode& barcode,const State& state);
void drawBarcode(QImage& image, State& state, const Barcode& barcode, const FieldData& field, const RenderOptions& options, QList<Diagnostic>& diagnostics,RenderFonts& fonts);

} // namespace QtZpl::Rendering
