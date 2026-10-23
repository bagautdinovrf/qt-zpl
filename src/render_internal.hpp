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
  // The label's final ^PO/^PM, applied to the whole raster after its fields.
  Orientation outputOrientation = Orientation::Normal;
  bool outputMirror = false;
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

// A Font 0 glyph image filled once with Qt's aliased rule at an integer pen
// position. Nonzero Alpha8 values are ink; offset is the image's top-left
// relative to the pen on the baseline.
struct Font0GlyphMask {
  QImage image;
  QPoint offset;
};
struct Font0PlacedGlyph {
  Font0GlyphMask mask;
  QPoint pen;
};
// Write ink wherever a placed mask is set: the union of the glyph images,
// clipped to target, which must be Format_ARGB32_Premultiplied.
void paintFont0Glyphs(QImage& target,QPoint origin,const QList<Font0PlacedGlyph>& glyphs,QRgb ink);

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
  // Small Font 0 glyph images do not depend on the pen phase, so one raster
  // serves every occurrence in this render call. Like Font A, the cache is
  // bounded and never retains large glyphs; misses are simply rebuilt.
  template<typename BuildPath>
  Font0GlyphMask font0Mask(quint32 glyph,int width,int height,BuildPath buildPath) {
    const Font0MaskKey key{glyph,width,height};
    if(const auto found=font0Masks_.constFind(key);found!=font0Masks_.cend())return found.value();
    Font0GlyphMask mask=rasterizeFont0Glyph(buildPath());
    const qsizetype bytes=mask.image.sizeInBytes();
    if(font0Masks_.size()<4096&&bytes<=64*1024&&font0MaskBytes_+bytes<=4*1024*1024){
      font0MaskBytes_+=bytes;
      font0Masks_.insert(key,mask);
    }
    return mask;
  }
  // The ARGB32 premultiplied pixel that Qt's aliased fill writes for color
  // over a transparent pixel, and over any pixel when color is opaque.
  QRgb font0InkPixel(const QColor& color);
  // Called for every Font A glyph: keep the loaded case inline.
  PrinterFontRaster& fontARaster() {
    if(!fontARaster_)loadFontARaster();
    return *fontARaster_;
  }
private:
  struct Font0MaskKey {
    quint32 glyph;
    int width;
    int height;
    friend bool operator==(const Font0MaskKey&,const Font0MaskKey&)=default;
    friend size_t qHash(const Font0MaskKey& key,size_t seed=0) noexcept {
      return qHashMulti(seed,key.glyph,key.width,key.height);
    }
  };
  void loadFontARaster();
  static const QByteArray& font0Bytes();
  Font0GlyphMask rasterizeFont0Glyph(const QPainterPath& path);
  std::optional<Font0Face> font0_;
  std::optional<PrinterFontRaster> fontARaster_;
  QHash<Font0MaskKey,Font0GlyphMask> font0Masks_;
  qsizetype font0MaskBytes_=0;
  QHash<quint64,QRgb> font0InkPixels_;
  // A new image and paint engine per glyph cost more than its fill, so small
  // masks are filled in one reusable scratch raster. The painter is declared
  // after its image and is therefore destroyed first.
  QImage maskScratch_;
  std::optional<QPainter> maskPainter_;
  QString font0Error_;
};

// Font 0 fields taller than this many dots are filled as outlines; smaller
// ones use phase-independent glyph images placed at integer pen positions.
inline constexpr int font0OutlineThreshold=100;

// Labelary samples pixels and rounds pen ties in the output raster. A field
// is drawn unrotated, then rotated with its field orientation and the whole
// label with ^PO and ^PM; these flags say which local axes run backwards.
struct OutputAxes {
  bool reverseX=false;
  bool reverseY=false;
};
OutputAxes outputAxes(Orientation field,Orientation print=Orientation::Normal,bool mirror=false);

// Rectangular Font 0 hyphen (U+002D) measured from Labelary references.
QRectF font0HyphenBounds(qreal penX,qreal penY,int width,int height,OutputAxes axes);
// Prepare a tall Font 0 outline in field coordinates for Qt's aliased fill:
// quantize it to the 1/64 dot grid and shift it so output-raster pixel
// corners are sampled, like the edges of font0HyphenBounds.
void sampleFont0OutlineCorners(QPainterPath& path,OutputAxes axes);

// Use the font's design grid rather than QFont::setStretch. On Windows the
// latter quantizes widths through GDI's integer average-character width, while
// other Qt backends use a different scale. Font 0 has a 1000-unit em and a
// 750-unit cap height; independent ZPL height/width scale that same design grid.
class Font0Metrics {
public:
  Font0Metrics(int height,int width,RenderFonts& fontContext,FieldParameter parameter={},
               Orientation orientation=Orientation::Normal,Orientation print=Orientation::Normal,
               bool mirror=false)
    : font(fontContext.font0()),fonts(fontContext),
      fontWidth(std::max(10,width>0?width:height)),scaleX(qreal(fontWidth)/1000),
      scaleY(static_cast<qreal>(std::max(10,height))/1000.0),fontHeight(std::max(10,height)),
      parameter(parameter),axes(outputAxes(orientation,print,mirror)) {}

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
  // Labelary draws Font 0 at most 100 dots high as glyph images: a glyph's
  // pixels do not depend on its fractional pen phase, and it is placed at
  // the pen rounded half towards the lower output coordinate. Taller text
  // fills the outline at the exact pen position, sampling output-pixel
  // corners instead of centres. Both rules are measured on the
  // tests/golden/font0-metrics rulers and font0-size-sweep pages.
  [[nodiscard]] bool usesGlyphImages() const {return fontHeight<=font0OutlineThreshold;}
  [[nodiscard]] QPainterPath outline(const QString& text) const {
    // TrueType contours use the nonzero rule. Even-odd filling would erase
    // overlapping contours and the overlap of adjacent glyphs.
    QPainterPath result;result.setFillRule(Qt::WindingFill);
    forEachGlyph(text,[&](quint32 glyph,qreal penX,qreal y){
      if(glyph==16)result.addRect(font0HyphenBounds(penX,y,fontWidth,fontHeight,axes));
      else{
        QTransform transform;transform.translate(penX,y);transform.scale(scaleX,scaleY);
        QPainterPath path=transform.map(fonts.font0Glyph(glyph));
        if(!usesGlyphImages())sampleFont0OutlineCorners(path,axes);
        result.addPath(path);
      }
    });
    return result;
  }
  // The same glyphs as outline() for glyph-image sizes, as cached masks at
  // integer pen positions relative to the field's baseline origin.
  [[nodiscard]] QList<Font0PlacedGlyph> glyphImages(const QString& text) const {
    QList<Font0PlacedGlyph> result;result.reserve(text.size());
    forEachGlyph(text,[&](quint32 glyph,qreal penX,qreal y){
      result.append({glyphMask(glyph),QPoint(static_cast<int>(penX),static_cast<int>(y))});
    });
    return result;
  }
  void draw(QImage& target,int x,int baseline,const QString& text,const QColor& color) const {
    if(usesGlyphImages()){paintFont0Glyphs(target,QPoint(x,baseline),glyphImages(text),fonts.font0InkPixel(color));return;}
    QPainter painter(&target);painter.setRenderHint(QPainter::Antialiasing,false);
    painter.fillPath(outline(text).translated(x,baseline),color);
  }
private:
  // Visit each glyph with its pen position: rounded for glyph images, exact
  // for outlines. Reverse fields start at the nominal M-cell edge and each
  // character's own width is subtracted before painting it.
  template<typename Visit>
  void forEachGlyph(const QString& text,Visit visit) const {
    const auto glyphs=font.glyphIndexesForString(text);
    qreal x=parameter.direction==u'R'?cellWidth():0,y=0;
    for(const auto glyph:glyphs){
      const qreal advance=fonts.font0Advance(glyph,fontWidth)+parameter.spacing;
      if(parameter.direction==u'R')x-=advance;
      visit(glyph,!usesGlyphImages()?x:axes.reverseX?std::floor(x+0.5):std::ceil(x-0.5),y);
      if(parameter.direction==u'V')y+=fontHeight;
      else if(parameter.direction!=u'R')x+=advance;
    }
  }
  [[nodiscard]] Font0GlyphMask glyphMask(quint32 glyph) const {
    return fonts.font0Mask(glyph,fontWidth,fontHeight,[&]{
      QPainterPath path;path.setFillRule(Qt::WindingFill);
      if(glyph==16)path.addRect(font0HyphenBounds(0,0,fontWidth,fontHeight,axes));
      else{QTransform transform;transform.scale(scaleX,scaleY);path.addPath(transform.map(fonts.font0Glyph(glyph)));}
      return path;
    });
  }
  const Font0Face& font;
  RenderFonts& fonts;
  int fontWidth;
  qreal scaleX;
  qreal scaleY;
  int fontHeight;
  FieldParameter parameter;
  OutputAxes axes;
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
