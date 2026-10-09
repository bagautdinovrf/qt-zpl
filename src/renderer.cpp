#include <QtZpl/qtzpl.hpp>
#include "barcode_encoders.hpp"
#include "pdf417_encoder.hpp"
#include "maxicode_encoder.hpp"
#include "linear_encoders.hpp"
#include "aztec_encoder.hpp"
#include "databar_encoder.hpp"
#include "micropdf417_encoder.hpp"
#include "printer_font_metrics.hpp"
#include "printer_font_raster.hpp"
#include "retail_font.hpp"
#include "graphic_geometry.hpp"
#include "font0_advance_data.hpp"
#include "font0_face.hpp"

#include <QtCore/QByteArray>
#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QtMath>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QFontDatabase>
#include <QtGui/QTransform>
#include <algorithm>
#include <bit>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
#include <utility>

static void initializeQtZplResources() {
  Q_INIT_RESOURCE(qtzpl_fonts);
}

namespace QtZpl {
namespace {
using namespace Qt::StringLiterals;

QPoint printerPosition(qint64 x,qint64 y) {
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
  ScalableFont font{u'A',Orientation::Normal,9,5};
  ScalableFont defaultFont{u'A',Orientation::Normal,9,5};
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

void observeLogical(const State& state,const QRect& bounds) {
  if(state.observation&&!bounds.isEmpty())state.observation->logicalBounds|=bounds;
}

void observePaint(const State& state,const QRect& bounds) {
  if(state.observation&&!bounds.isEmpty())state.observation->paintBounds|=bounds;
}

QColor commandColor(LineColor color, const RenderOptions& options) {
  return color == LineColor::White ? options.background : options.foreground;
}

QString embeddedFontFamily(QStringView resourcePath) {
  const int id=QFontDatabase::addApplicationFont(resourcePath.toString());
  const auto families=QFontDatabase::applicationFontFamilies(id);
  return families.isEmpty()?QString{}:families.front();
}

QString zebraFontFamily() {
  static const QString family=[] {
    initializeQtZplResources();
    return embeddedFontFamily(u":/qtzpl/fonts/font0.ttf");
  }();
  return family;
}

QString fontAFamily() {
  static const QString family=[] {
    initializeQtZplResources();
    return embeddedFontFamily(u":/qtzpl/fonts/font_a.ttf");
  }();
  return family;
}

QString ocrBFontFamily() {
  static const QString family=[] {
    initializeQtZplResources();
    return embeddedFontFamily(u":/qtzpl/fonts/ocr_b.ttf");
  }();
  return family;
}

QFont printerFont(const QString& family,int pixelHeight,int stretch=100) {
  QFont font(family);
  font.setPixelSize(std::max(1,pixelHeight));
  font.setHintingPreference(QFont::PreferFullHinting);
  font.setStyleStrategy(QFont::NoAntialias);
  font.setStretch(std::clamp(stretch,1,400));
  return font;
}

QFont zebraFont(int pixelHeight, int stretch=75) {
  return printerFont(zebraFontFamily(),pixelHeight,stretch);
}

QFont builtInFont(QChar name,int pixelHeight,int requestedWidth=0) {
  if(name.toUpper()==u'A') {
    const int stretch=requestedWidth>0
      ? std::clamp(qRound(180.0*requestedWidth/pixelHeight),1,400)
      : 100;
    return printerFont(fontAFamily(),pixelHeight,stretch);
  }
  constexpr int zebraFont0NaturalStretch=77;
  const int stretch=requestedWidth>0
    ? std::clamp(zebraFont0NaturalStretch*requestedWidth/pixelHeight,1,400)
    : zebraFont0NaturalStretch;
  return zebraFont(pixelHeight,stretch);
}

QFont eanInterpretationFont(int pixelHeight) {
  static const QString family=ocrBFontFamily();
  return printerFont(family,pixelHeight);
}

QSize builtInFontDefaults(QChar font) {
  switch (font.toUpper().unicode()) {
    case u'A': return {5,9};
    case u'B': return {7,11};
    case u'C':
    case u'D': return {10,18};
    case u'E': return {15,28};
    case u'F': return {13,26};
    case u'G': return {40,60};
    case u'H': return {13,21};
    default: return {0,30};
  }
}

QSize resolvedFontSize(const ScalableFont& font) {
  const auto defaults=builtInFontDefaults(font.font);
  const int height=font.height>0?font.height:defaults.height();
  const int width=font.width>0?font.width:
    font.font.toUpper()==u'A'?std::max(1,qRound(height/9.0))*5:defaults.width();
  // Font 0 clamps each axis independently. The effective cell size also
  // controls FO/FT anchors and rotation, not just the glyph outline scale.
  if(font.font==u'0')return {std::max(10,width>0?width:height),std::max(10,height)};
  return {width,height};
}

QImage rotateImage(const QImage& source, Orientation orientation) {
  QTransform transform;
  switch (orientation) {
    case Orientation::Rotated90: transform.rotate(90); break;
    case Orientation::Inverted: transform.rotate(180); break;
    case Orientation::BottomUp: transform.rotate(270); break;
    default: return source;
  }
  return source.transformed(transform, Qt::FastTransformation);
}

QRect alphaBounds(const QImage& image) {
  QRect bounds;
  for(int y=0;y<image.height();++y) {
    const auto* pixels=reinterpret_cast<const QRgb*>(image.constScanLine(y));
    for(int x=0;x<image.width();++x)
      if(qAlpha(pixels[x])!=0)bounds|=QRect(x,y,1,1);
  }
  return bounds;
}

void observeRaster(const State& state,QPoint position,const QImage& raster,bool logical=true) {
  if(!state.observation)return;
  if(logical)observeLogical(state,QRect(position,raster.size()));
  const auto bounds=alphaBounds(raster);
  if(!bounds.isEmpty())observePaint(state,bounds.translated(position));
}

struct BlockLine {
  QString text;
  int indent = 0;
  bool paragraphEnd = false;
};

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
  PrinterFontRaster& fontARaster() {
    if(!fontARaster_){
      static const QByteArray bytes=[] {
        initializeQtZplResources();QFile file(u":/qtzpl/fonts/font_a_native.ttf"_s);
        return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};
      }();
      fontARaster_.emplace(bytes);
    }
    return *fontARaster_;
  }
private:
  static const QByteArray& font0Bytes() {
    static const QByteArray bytes=[] {
      initializeQtZplResources();
      QFile file(u":/qtzpl/fonts/font0.ttf"_s);
      return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};
    }();
    return bytes;
  }
  std::optional<Font0Face> font0_;
  std::optional<PrinterFontRaster> fontARaster_;
  QString font0Error_;
};

// Use the font's design grid rather than QFont::setStretch. On Windows the
// latter quantizes widths through GDI's integer average-character width, while
// other Qt backends use a different scale. Font 0 has a 1000-unit em and a
// 750-unit cap height; independent ZPL height/width scale that same design grid.
class Font0Metrics {
public:
  Font0Metrics(int height,int width,RenderFonts& fontContext,FieldParameter parameter={})
    : font(fontContext.font0()),fonts(fontContext),
      fontWidth(std::max(10,width>0?width:height)),scaleX(qreal(fontWidth)/1000),
      scaleY(static_cast<qreal>(std::max(10,height))/1000.0),fontHeight(std::max(10,height)),parameter(parameter) {}

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
      QTransform transform;transform.translate(x,y);transform.scale(scaleX,scaleY);
      result.addPath(transform.map(fonts.font0Glyph(glyphs[i])));
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
};

template<typename Metrics>
QList<BlockLine> layoutBlockLines(const QString& text,const FieldBlock& block,const Metrics& metrics) {
  QList<BlockLine> lines;
  const int maximumLines=std::max(1,block.maxLines);
  const int hangingIndent=std::max(0,block.hangingIndent);
  bool full=false;
  const auto appendLine=[&](QString line,bool paragraphEnd){
    if(full)return;
    lines.append({std::move(line),lines.isEmpty()?0:hangingIndent,paragraphEnd});
    full=lines.size()>=maximumLines;
  };

  const auto paragraphs=text.split(u"\\&"_s,Qt::KeepEmptyParts);
  for(const auto& paragraph:paragraphs){
    if(full)break;
    QString current;
    const auto words=paragraph.split(u' ',Qt::SkipEmptyParts);
    for(const auto& word:words){
      if(full)break;
      const int indent=lines.isEmpty()?0:hangingIndent;
      const int available=std::max(1,block.width-indent);
      const QString candidate=current.isEmpty()?word:current+u' '+word;
      if(metrics.horizontalAdvance(candidate)<=available){
        current=candidate;
        continue;
      }
      if(!current.isEmpty()){
        appendLine(std::move(current),false);
        current.clear();
        if(full)break;
      }

      QString remaining=word;
      while(!remaining.isEmpty()&&!full){
        const int continuationIndent=lines.isEmpty()?0:hangingIndent;
        const int continuationWidth=std::max(1,block.width-continuationIndent);
        if(metrics.horizontalAdvance(remaining)<=continuationWidth){
          current=remaining;
          break;
        }
        int split=1;
        while(split<remaining.size()
              &&metrics.horizontalAdvance(remaining.left(split+1)+u'-')<=continuationWidth)
          ++split;
        appendLine(remaining.left(split)+u'-',false);
        remaining.remove(0,split);
      }
    }
    if(!full)appendLine(std::move(current),true);
  }
  if(!lines.isEmpty())lines.back().paragraphEnd=true;
  return lines;
}

template<typename Metrics>
int logicalAlignmentX(const BlockLine& line,const FieldBlock& block,const Metrics& metrics) {
  const int textWidth=metrics.horizontalAdvance(line.text);
  const int available=std::max(0,block.width-line.indent);
  if(block.justification==Justification::Right)return line.indent+std::max(0,available-textWidth);
  if(block.justification==Justification::Center)return line.indent+std::max(0,(available-textWidth)/2);
  return line.indent;
}

QPoint rotatedAnchor(QPoint anchor,QSize source,Orientation orientation) {
  switch(orientation){
    case Orientation::Rotated90:return {source.height()-1-anchor.y(),anchor.x()};
    case Orientation::Inverted:return {source.width()-1-anchor.x(),source.height()-1-anchor.y()};
    case Orientation::BottomUp:return {anchor.y(),source.width()-1-anchor.x()};
    default:return anchor;
  }
}

// Baselines and barcode/graphic anchors lie on cell edges. Rotating an edge
// uses the full extent, unlike rotating the centre of an individual dot.
QPoint rotatedEdge(QPoint anchor,QSize source,Orientation orientation) {
  switch(orientation){
    case Orientation::Rotated90:return {source.height()-anchor.y(),anchor.x()};
    case Orientation::Inverted:return {source.width()-anchor.x(),source.height()-anchor.y()};
    case Orientation::BottomUp:return {anchor.y(),source.width()-anchor.x()};
    default:return anchor;
  }
}

QPoint font0OriginBaseline(int height,int ascent,int cellWidth,int count,qreal fieldAdvance,
                          QChar direction,Orientation orientation) {
  // FO rotates the nominal text cell, whereas FT names the baseline directly.
  // Vertical and reverse fields have a one-character cell across the baseline.
  const int span=direction==u'H'?qFloor(fieldAdvance):cellWidth;
  switch(orientation){
    case Orientation::Rotated90:return {direction==u'V'?(count-1)*height:height-ascent,0};
    case Orientation::Inverted:return {span,(direction==u'V'?count*height:height)-ascent};
    case Orientation::BottomUp:return {ascent,span};
    default:return {0,ascent};
  }
}

void placeNewSymbol(QImage& image,const QImage& original,const State& state,Orientation orientation);

template<typename Metrics,typename PaintText>
void drawBlockTextLayout(QImage& image,State& state,const QString& text,const RenderOptions& options,
                        const Metrics& metrics,Orientation orientation,PaintText paintText) {
  const auto block=*state.block;
  const auto lines=layoutBlockLines(text,block,metrics);
  if(lines.isEmpty()){state.reverse=false;state.block.reset();return;}
  const int lineAdvance=std::max(1,metrics.height()+block.lineSpacing);
  const int baseline=metrics.ascent();
  const int actualHeight=std::max(
      1,metrics.height()+static_cast<int>(lines.size()-1)*lineAdvance);
  const int layoutLineCount=state.baseline?std::max(1,block.maxLines):static_cast<int>(lines.size());
  const int layoutHeight=std::max(1,metrics.height()+(layoutLineCount-1)*lineAdvance);
  QImage field(std::max(1,block.width),actualHeight,QImage::Format_ARGB32_Premultiplied);
  field.fill(Qt::transparent);
  QPainter fp(&field);fp.setRenderHint(QPainter::TextAntialiasing,false);
  fp.setRenderHint(QPainter::Antialiasing,false);
  fp.setPen(state.reverse?options.background:options.foreground);
  for(qsizetype lineIndex=0;lineIndex<lines.size();++lineIndex){
    const auto& line=lines[lineIndex];
    const int y=baseline+static_cast<int>(lineIndex)*lineAdvance;
    if(block.justification==Justification::Justified&&!line.paragraphEnd){
      const auto words=line.text.split(u' ',Qt::SkipEmptyParts);
      if(words.size()>1){
        int wordsWidth=0;for(const auto& word:words)wordsWidth+=metrics.horizontalAdvance(word);
        const double gap=static_cast<double>(std::max(0,block.width-line.indent-wordsWidth))/(words.size()-1);
        double x=line.indent;for(const auto& word:words){paintText(fp,qRound(x),y,word);x+=metrics.horizontalAdvance(word)+gap;}
        continue;
      }
    }
    paintText(fp,logicalAlignmentX(line,block,metrics),y,line.text);
  }
  fp.end();

  const QPoint logicalAnchor=state.baseline
    ?QPoint(0,baseline+(layoutLineCount-1)*lineAdvance)
    :QPoint{};
  const QSize layoutSize(field.width(),layoutHeight);
  // ^FO positions the top-left of the rotated block. Only ^FT names a
  // baseline that must itself be rotated around the logical field origin.
  const QPoint anchor=!state.baseline?QPoint{}:
    std::is_same_v<Metrics,Font0Metrics>?rotatedEdge(logicalAnchor,layoutSize,orientation):
      rotatedAnchor(logicalAnchor,layoutSize,orientation);
  QPoint rasterOffset;
  if(orientation==Orientation::Rotated90)rasterOffset.rx()=layoutHeight-actualHeight;
  else if(orientation==Orientation::Inverted)rasterOffset.ry()=layoutHeight-actualHeight;
  field=rotateImage(field,orientation);
  const auto position=state.position-anchor+rasterOffset;
  const QSize logicalSize=(orientation==Orientation::Rotated90||orientation==Orientation::BottomUp)
    ?QSize(layoutSize.height(),layoutSize.width()):layoutSize;
  observeLogical(state,QRect(state.position-anchor,logicalSize));
  observeRaster(state,position,field,false);
  QPainter painter(&image);
  painter.setCompositionMode(state.reverse?QPainter::CompositionMode_Difference:QPainter::CompositionMode_SourceOver);
  painter.drawImage(position,field);
  state.reverse=false;state.block.reset();
}

void drawBlockText(QImage& image,State& state,const QString& text,const RenderOptions& options,
                   const QFont& font,Orientation orientation) {
  drawBlockTextLayout(image,state,text,options,QFontMetrics(font),orientation,
    [&](QPainter& painter,int x,int y,const QString& line){painter.setFont(font);painter.drawText(x,y,line);});
}

void drawFont0Text(QImage& image,State& state,const QString& text,const RenderOptions& options,
                   int height,int width,Orientation orientation,RenderFonts& fonts) {
  const Font0Metrics metrics(height,width,fonts,state.parameter);
  const QColor color=state.reverse?options.background:options.foreground;
  if(state.block&&state.block->width>0){
    drawBlockTextLayout(image,state,text,options,metrics,orientation,
      [&](QPainter& painter,int x,int y,const QString& line){metrics.draw(painter,x,y,line,color);});
    return;
  }

  const QPainterPath outline=metrics.outline(text);
  QPoint baselinePosition=state.position;
  if(!state.baseline){
    const qreal span=orientation==Orientation::Inverted||orientation==Orientation::BottomUp
      ?metrics.designAdvance(text)+state.parameter.spacing:0;
    const int count=state.parameter.direction==u'V'?int(text.toUcs4().size()):0;
    baselinePosition+=font0OriginBaseline(height,metrics.ascent(),metrics.cellWidth(),count,span,
                                          state.parameter.direction,orientation);
  }
  const QRect bounds=outline.boundingRect().toAlignedRect();
  if(bounds.isEmpty()){state.reverse=false;state.block.reset();return;}
  // An opaque, unrotated field wholly on the canvas can use the same aliased
  // path directly, avoiding a temporary image, its clear, and its composition.
  // Preserve the cropped path's local coordinates and integer placement.
  // Keep the image path for clipping, alpha, reverse, and pixel-bound observers;
  // direct raster-engine coordinates also stay inside Qt's +/- 2^15 range.
  const qint64 left=qint64(baselinePosition.x())+bounds.left();
  const qint64 top=qint64(baselinePosition.y())+bounds.top();
  constexpr int maximumPainterCoordinate=32767;
  if(orientation==Orientation::Normal&&!state.reverse&&!state.observation
     &&color.rgba64().alpha()==65535&&left>=0&&top>=0
     &&left+bounds.width()<=std::min(image.width(),maximumPainterCoordinate)
     &&top+bounds.height()<=std::min(image.height(),maximumPainterCoordinate)){
    QPainter painter(&image);painter.setRenderHint(QPainter::Antialiasing,false);
    painter.translate(int(left),int(top));
    painter.fillPath(outline.translated(-bounds.left(),-bounds.top()),color);
    state.reverse=false;state.block.reset();return;
  }
  QImage field(bounds.size(),QImage::Format_ARGB32_Premultiplied);
  field.fill(Qt::transparent);
  QPainter raster(&field);raster.setRenderHint(QPainter::Antialiasing,false);
  raster.fillPath(outline.translated(-bounds.left(),-bounds.top()),color);raster.end();

  const QPoint position=baselinePosition-rotatedEdge(-bounds.topLeft(),field.size(),orientation);
  field=rotateImage(field,orientation);
  observeRaster(state,position,field,false);
  QPainter painter(&image);
  painter.setCompositionMode(state.reverse?QPainter::CompositionMode_Difference:QPainter::CompositionMode_SourceOver);
  painter.drawImage(position,field);
  state.reverse=false;state.block.reset();
}

QPoint orientedVector(QPoint vector,Orientation orientation) {
  switch(orientation){
    case Orientation::Rotated90:return {-vector.y(),vector.x()};
    case Orientation::Inverted:return {-vector.x(),-vector.y()};
    case Orientation::BottomUp:return {vector.y(),-vector.x()};
    default:return vector;
  }
}

QPoint orientedTextAdvance(qreal advance,Orientation orientation) {
  // Quantize after orientation: negative advances need floor, not truncation
  // or the negation of an already rounded positive width.
  switch(orientation){
    case Orientation::Rotated90:return {0,qFloor(advance)};
    case Orientation::Inverted:return {qFloor(-advance),0};
    case Orientation::BottomUp:return {0,qFloor(-advance)};
    default:return {qFloor(advance),0};
  }
}

void paintPrinterGlyph(QPainter& painter,QPoint baseline,const PrinterGlyph& glyph,const QColor& ink) {
  if(glyph.alpha.isNull())return;
  QImage colored(glyph.alpha.size(),QImage::Format_ARGB32_Premultiplied);
  if(colored.isNull())return;
  const QRgb foreground=qPremultiply(ink.rgba());
  for(int y=0;y<colored.height();++y){
    const auto* alpha=glyph.alpha.constScanLine(y);
    auto* pixels=reinterpret_cast<QRgb*>(colored.scanLine(y));
    for(int x=0;x<colored.width();++x)pixels[x]=alpha[x]?foreground:0;
  }
  painter.drawImage(baseline+glyph.bearing,colored);
}

void paintPrinterFontAGlyph(QPainter& painter,QPoint baseline,QChar character,int height,int width,
                           const QColor& ink,RenderFonts& fonts,Orientation orientation=Orientation::Normal) {
  const int fontWidth=std::max(1,qRound(width/5.0))*9;
  const int fontHeight=std::max(1,qRound(height/9.0))*9;
  const int turns=orientation==Orientation::Rotated90?1:orientation==Orientation::Inverted?2:orientation==Orientation::BottomUp?3:0;
  const auto glyph=fonts.fontARaster().glyph(PrinterFontMetrics::fontACharacter(character.unicode()),fontWidth,fontHeight,turns);
  if(glyph)paintPrinterGlyph(painter,baseline,*glyph,ink);
}

void drawFontAText(QImage& image,State& state,const QString& text,const RenderOptions& options,
                   int height,int width,Orientation orientation,RenderFonts& fonts,
                   QList<Diagnostic>& diagnostics,qsizetype offset) {
  const int fontWidth=std::max(1,qRound(width/5.0))*9;
  const int fontHeight=std::max(1,qRound(height/9.0))*9;
  const qreal advance=fontWidth*1365.0/2048;
  const int ascent=qRound(fontHeight*1583.0/2048);
  const int turns=orientation==Orientation::Rotated90?1:orientation==Orientation::Inverted?2:orientation==Orientation::BottomUp?3:0;
  struct PositionedGlyph { PrinterGlyph glyph;QPoint baseline; };
  QList<PositionedGlyph> glyphs;
  QRect bounds;qreal x=state.parameter.direction==u'R'?-state.parameter.spacing:0,y=0;
  for(const char32_t character:text.toUcs4()){
    const auto glyph=fonts.fontARaster().glyph(PrinterFontMetrics::fontACharacter(character),fontWidth,fontHeight,turns);
    if(!glyph){
      diagnostics.append({Severity::Warning,u"font-glyph-missing"_s,glyph.error(),offset,u"^FD"_s});
      if(diagnostics.size()>options.maxDiagnostics)return;
      continue;
    }
    const QPoint baseline(qRound(x),qRound(y)+(state.baseline?0:ascent));
    bounds|=QRect(baseline+glyph->bearing,glyph->alpha.size());
    glyphs.append({*glyph,baseline});
    if(state.parameter.direction==u'V')y+=height;
    else x+=(state.parameter.direction==u'R'?-1:1)*(advance+state.parameter.spacing);
  }
  if(bounds.isEmpty())return;
  QImage field(bounds.size(),QImage::Format_ARGB32_Premultiplied);field.fill(Qt::transparent);
  QPainter painter(&field);painter.setRenderHint(QPainter::Antialiasing,false);
  for(const auto& glyph:glyphs)
    paintPrinterGlyph(painter,glyph.baseline-bounds.topLeft(),glyph.glyph,state.reverse?Qt::white:options.foreground);
  painter.end();
  const int span=qFloor(fontWidth*1365.0/2048*text.size()+state.parameter.spacing*std::max(0,int(text.size())-1));
  QPoint position=state.position;
  if(state.baseline)position-=rotatedEdge(-bounds.topLeft(),field.size(),orientation);
  else switch(orientation){
    case Orientation::Normal:position+=bounds.topLeft();break;
    case Orientation::Rotated90:position+=QPoint(height-bounds.bottom()-1,bounds.left());break;
    case Orientation::Inverted:position+=QPoint(span-bounds.right()-1,height-bounds.bottom()-1);break;
    case Orientation::BottomUp:position+=QPoint(bounds.top(),span-bounds.right()-1);break;
  }
  const auto rotated=rotateImage(field,orientation);
  observeRaster(state,position,rotated,false);
  QPainter target(&image);if(state.reverse)target.setCompositionMode(QPainter::CompositionMode_Difference);
  target.drawImage(position,rotated);
}

void drawTextField(QImage& image, State& state, const QString& text, const RenderOptions& options,RenderFonts& fonts,
                   QList<Diagnostic>& diagnostics,qsizetype offset) {
  if (text.isEmpty()) { state.reverse=false; state.block.reset(); return; }
  const QSize size=resolvedFontSize(state.font);
  const int height=size.height(),width=size.width();
  const auto orientation=state.font.orientation==Orientation::Normal?state.fieldDirection:state.font.orientation;
  if(state.font.font==u'0'){
    drawFont0Text(image,state,text,options,height,width,orientation,fonts);
    return;
  }
  const bool fontA=state.font.font.toUpper()==u'A';
  if(fontA&&!state.block){drawFontAText(image,state,text,options,height,width,orientation,fonts,diagnostics,offset);return;}
  // Font A dimensions select integer multiples of its nominal 5 x 9 cell.
  const int pixelHeight=fontA?std::max(1,qRound(height/9.0))*10:std::max(1,height-1);
  constexpr int zebraFont0NaturalStretch=77;
  const int requestedStretch=width>0
    ? zebraFont0NaturalStretch*width/height
    : zebraFont0NaturalStretch;
  QFont font=fontA
    ? printerFont(fontAFamily(),pixelHeight,std::clamp(qRound(1000.0*std::max(1,qRound(width/5.0))/pixelHeight),1,400))
    : zebraFont(pixelHeight,requestedStretch);
  if(state.parameter.direction==u'H'&&state.parameter.spacing!=0)
    font.setLetterSpacing(QFont::AbsoluteSpacing,state.parameter.spacing);

  if(state.block&&state.block->width>0){drawBlockText(image,state,text,options,font,orientation);return;}

  QFontMetrics metrics(font);
  QPainterPath directedText;
  if(state.parameter.direction!=u'H'){
    qreal x=state.parameter.direction==u'R'?-state.parameter.spacing:0,y=0;
    for(QChar c:text){
      directedText.addText(QPointF(x,y),font,QString(c));
      if(state.parameter.direction==u'V')y+=height;
      else x-=metrics.horizontalAdvance(c)+state.parameter.spacing;
    }
  }
  const auto bounds = (directedText.isEmpty()?metrics.boundingRect(text):directedText.boundingRect().toAlignedRect()).adjusted(-1,-1,1,1);
  QImage field(std::max(1,bounds.width()), std::max(1,bounds.height()), QImage::Format_ARGB32_Premultiplied);
  field.fill(Qt::transparent);
  QPainter fp(&field);
  fp.setRenderHint(QPainter::TextAntialiasing, false);
  fp.setFont(font);
  fp.setPen(state.reverse ? options.background : options.foreground);
  if(directedText.isEmpty())fp.drawText(-bounds.left(), -bounds.top(), text);
  else fp.fillPath(directedText.translated(-bounds.left(),-bounds.top()),state.reverse?options.background:options.foreground);
  fp.end();

  const QRect ink=alphaBounds(field);

  const int baselineX = -bounds.left();
  const int baselineY = -bounds.top();
  field = rotateImage(field, orientation);
  QPoint pos = state.position;
  if(fontA&&!state.baseline&&orientation==Orientation::Normal&&!ink.isEmpty()){
    pos.ry()-=ink.top();
    if(state.parameter.direction!=u'H')pos.rx()+=bounds.left();
  }
  else if(!state.baseline&&orientation==Orientation::Normal)pos.rx()+=bounds.left();
  if (state.baseline) {
    // ^FT names the baseline origin. Rotate that point with the glyph raster so
    // R/I/B fields remain anchored at ^FT instead of receiving the N-only
    // ascent adjustment after rotation.
    switch (orientation) {
      case Orientation::Normal:
        pos -= QPoint(baselineX, baselineY);
        break;
      case Orientation::Rotated90:
        pos -= QPoint(bounds.height() - 1 - baselineY, baselineX);
        break;
      case Orientation::Inverted:
        pos -= QPoint(bounds.width() - 1 - baselineX, bounds.height() - 1 - baselineY);
        break;
      case Orientation::BottomUp:
        pos -= QPoint(baselineY, bounds.width() - 1 - baselineX);
        break;
    }
  }
  QPainter painter(&image);
  observeRaster(state,pos,field,false);
  painter.setCompositionMode(state.reverse ? QPainter::CompositionMode_Difference : QPainter::CompositionMode_SourceOver);
  painter.drawImage(pos, field);
  state.reverse=false; state.block.reset();
}

void drawText(QImage& image,State& state,const QString& text,const RenderOptions& options,RenderFonts& fonts,
              QList<Diagnostic>& diagnostics,qsizetype offset) {
  const QSize size=resolvedFontSize(state.font);
  const int height=size.height(),width=size.width();
  const qint64 nominalAdvance=2*qint64(std::max({std::min(height,32000),std::min(width,32000),1}))+std::abs(state.parameter.spacing);
  const qint64 maximumArea=nominalAdvance*std::clamp(height,1,32000)*std::min<qsizetype>(text.size(),64*1024*1024);
  bool unsafeBlock=false;
  if(state.block){
    const auto& block=*state.block;
    const qint64 lineHeight=std::max<qint64>(1,qint64(height)+block.lineSpacing);
    const qint64 blockHeight=qint64(height)+std::max<qint64>(0,qint64(block.maxLines)-1)*lineHeight;
    unsafeBlock=block.width>32000||block.maxLines>32000||blockHeight>32000||
      qint64(std::clamp(block.width,1,32000))*std::clamp<qint64>(blockHeight,1,32000)>64*1024*1024;
  }
  if(height>32000||width>32000||maximumArea>64*1024*1024||unsafeBlock){
    diagnostics.append({Severity::Error,u"text-size"_s,u"Text field exceeds the safe raster allocation limit."_s,offset,u"^FD"_s});return;
  }
  const auto orientation=state.font.orientation==Orientation::Normal?state.fieldDirection:state.font.orientation;
  const bool font0=state.font.font==u'0';
  int advance=0,ascent=height,font0CellWidth=0;
  qreal font0FieldAdvance=0;
  if(font0){
    Font0Metrics metrics(height,width,fonts,state.parameter);
    const qreal designAdvance=metrics.designAdvance(text);
    advance=qRound(designAdvance);ascent=metrics.ascent();font0CellWidth=text.isEmpty()?0:metrics.cellWidth();
    // Continuation and field justification include the trailing FP gap;
    // line wrapping keeps the separate inter-character-only advance.
    font0FieldAdvance=designAdvance+(text.isEmpty()?0:state.parameter.spacing);
  }else if(state.font.font.toUpper()==u'A'){
    advance=std::max(1,qRound(width/5.0))*6*int(text.size())+state.parameter.spacing*std::max(0,int(text.size())-1);
    if(orientation==Orientation::Inverted||orientation==Orientation::BottomUp)
      advance=qFloor(std::max(1,qRound(width/5.0))*9.0*1365.0/2048*text.size()+state.parameter.spacing*std::max(0,int(text.size())-1));
    ascent=qRound(std::max(1,qRound(height/9.0))*9.0*1583.0/2048);
  }else{
    QFontMetrics metrics(builtInFont(state.font.font,height,width));advance=metrics.horizontalAdvance(text);ascent=metrics.ascent();
  }
  State placed=state;
  const bool right=state.justification==Justification::Right||(state.justification==Justification::Auto&&text.isRightToLeft());
  if(right){
    if(font0&&!state.baseline&&state.parameter.direction==u'H'&&orientation!=Orientation::Normal){
      placed.position.rx()-=(orientation==Orientation::Rotated90||orientation==Orientation::BottomUp
                              ?height:qFloor(font0FieldAdvance));
    }else if(font0){
      const bool reverseOrigin=!state.baseline&&(orientation==Orientation::Inverted||orientation==Orientation::BottomUp);
      const qreal offset=text.isEmpty()?0:state.parameter.direction==u'V'?-height:
        state.parameter.direction==u'R'?font0FieldAdvance-(reverseOrigin?1:2)*font0CellWidth:-font0FieldAdvance;
      placed.position+=orientedTextAdvance(offset,orientation);
    }else if(state.baseline)placed.position-=orientedVector(QPoint(advance,0),orientation);
    else placed.position.rx()-=(orientation==Orientation::Rotated90||orientation==Orientation::BottomUp?height:advance);
  }
  const int cursorAdvance=state.font.font.toUpper()==u'A'
    ?qFloor(std::max(1,qRound(width/5.0))*9.0*1365.0/2048*text.size()+state.parameter.spacing*std::max(0,int(text.size())-1))
    :advance;
  const QPoint end=state.parameter.direction==u'V'?QPoint(0,height*int(text.size()))
    :QPoint(state.parameter.direction==u'R'?-cursorAdvance:cursorAdvance,0);
  if(font0){
    if(text.isEmpty())state.nextTypeset=state.position+orientedVector(QPoint(0,state.baseline?0:ascent),orientation);
    else if(state.baseline){
      const qreal cursor=font0FieldAdvance-(state.parameter.direction==u'R'?font0CellWidth:0);
      state.nextTypeset=placed.position+(right?QPoint{}:orientedTextAdvance(cursor,orientation));
    }else{
      const int count=state.parameter.direction==u'V'?int(text.toUcs4().size()):0;
      QPoint baseline=font0OriginBaseline(height,ascent,font0CellWidth,count,font0FieldAdvance,
                                          state.parameter.direction,orientation);
      if(state.parameter.direction==u'R'){
        const bool inverted=orientation==Orientation::Inverted||orientation==Orientation::BottomUp;
        if(orientation==Orientation::Inverted)baseline.rx()-=font0CellWidth;
        if(orientation==Orientation::BottomUp)baseline.ry()-=font0CellWidth;
        const qreal cursor=font0FieldAdvance-(inverted?(right?1:0):(right?2:1))*font0CellWidth;
        state.nextTypeset=state.position+baseline+orientedTextAdvance(cursor,orientation);
      }else state.nextTypeset=placed.position+baseline+(right?QPoint{}:orientedTextAdvance(font0FieldAdvance,orientation));
    }
  }else state.nextTypeset=placed.position+orientedVector(end+QPoint(0,state.baseline?0:ascent),orientation);
  if(state.observation&&(!state.block||state.block->width<=0)){
    const int logicalWidth=state.parameter.direction==u'V'?(width>0?width:height):std::abs(advance);
    const int logicalHeight=state.parameter.direction==u'V'?height*int(text.toUcs4().size()):height;
    QPoint origin=placed.position;
    if(state.baseline)origin-=orientedVector(QPoint(0,ascent),orientation);
    if(state.parameter.direction==u'R')origin+=orientedVector(QPoint(-logicalWidth,0),orientation);
    if(state.baseline){
      const QPoint opposite=origin+orientedVector(QPoint(logicalWidth,logicalHeight),orientation);
      observeLogical(state,QRect(QPoint(std::min(origin.x(),opposite.x()),std::min(origin.y(),opposite.y())),
        QSize(std::abs(opposite.x()-origin.x()),std::abs(opposite.y()-origin.y()))));
    }else observeLogical(state,QRect(origin,(orientation==Orientation::Rotated90||orientation==Orientation::BottomUp)
      ?QSize(logicalHeight,logicalWidth):QSize(logicalWidth,logicalHeight)));
  }
  if(!state.rasterize){state.reverse=false;state.block.reset();return;}
  drawTextField(image,placed,text,options,fonts,diagnostics,offset);
  state.reverse=false;state.block.reset();
}

void drawGraphicField(QImage& image,const QPoint& pos,const DecodedGraphic& decoded,const RenderOptions& options,
                      const State& state) {
  {
    const QByteArrayView bytes{decoded.bytes};
    observeLogical(state,QRect(pos,decoded.size));
    if(state.observation&&options.foreground.alpha()>0){
      QRect paint;
      for(int y=0;y<decoded.size.height();++y){
        int first=-1,last=-1;
        for(int x=0;x<decoded.bytesPerRow;++x){
          const auto byte=static_cast<unsigned char>(bytes[qsizetype(y)*decoded.bytesPerRow+x]);
          if(byte){if(first<0)first=x*8+std::countl_zero(byte);last=x*8+7-std::countr_zero(byte);}
        }
        if(first>=0)paint|=QRect(pos+QPoint(first,y),QSize(last-first+1,1));
      }
      observePaint(state,paint);
    }
    // SourceOver with fully opaque ink replaces the destination exactly.
    // Clip in wide arithmetic before requesting scanlines; input validation
    // above still processes the entire graphic, even when it is off-label.
    // QColor::alpha() rounds 16-bit alpha, so it cannot decide opacity here.
    if(options.foreground.rgba64().alpha()==65535){
      if(bytes.isEmpty())return;
      const qsizetype rowBytes=decoded.bytesPerRow;
      const qint64 firstX=std::max<qint64>(0,-static_cast<qint64>(pos.x()));
      const qint64 lastX=std::min<qint64>(static_cast<qint64>(rowBytes)*8,
        static_cast<qint64>(image.width())-pos.x());
      const qint64 firstY=std::max<qint64>(0,-static_cast<qint64>(pos.y()));
      const qint64 lastY=std::min<qint64>((bytes.size()-1)/rowBytes+1,
        static_cast<qint64>(image.height())-pos.y());
      if(firstX>=lastX||firstY>=lastY)return;
      const QRgb ink=options.foreground.rgba();
      for(qint64 y=firstY;y<lastY;++y){
        const qsizetype rowStart=static_cast<qsizetype>(y*rowBytes);
        const qint64 end=std::min(lastX,static_cast<qint64>(bytes.size()-rowStart)*8);
        auto* pixels=reinterpret_cast<QRgb*>(image.scanLine(static_cast<int>(pos.y()+y)));
        for(qint64 x=firstX;x<end;++x)
          if(static_cast<unsigned char>(bytes[rowStart+static_cast<qsizetype>(x/8)])&(1U<<(7-x%8)))
            pixels[static_cast<qsizetype>(pos.x()+x)]=ink;
      }
      return;
    }
    qint64 x=0,y=0;
    const qint64 rowWidth=decoded.size.width();
    QPainter painter(&image);painter.setPen(options.foreground);
    for(const unsigned char byte:bytes){
      for(int bit=7;bit>=0;--bit){
        if(byte&(1U<<bit)){
          const qint64 targetX=pos.x()+x,targetY=pos.y()+y;
          if(targetX>=0&&targetX<image.width()&&targetY>=0&&targetY<image.height())
            painter.drawPoint(static_cast<int>(targetX),static_cast<int>(targetY));
        }
        if(++x>=rowWidth){x=0;++y;}
      }
    }
    return;
  }
}

QString code39Pattern(QChar c) {
  static const QHash<QChar,QString> patterns{
    {u'0',u"nnnwwnwnn"_s},{u'1',u"wnnwnnnnw"_s},{u'2',u"nnwwnnnnw"_s},{u'3',u"wnwwnnnnn"_s},
    {u'4',u"nnnwwnnnw"_s},{u'5',u"wnnwwnnnn"_s},{u'6',u"nnwwwnnnn"_s},{u'7',u"nnnwnnwnw"_s},
    {u'8',u"wnnwnnwnn"_s},{u'9',u"nnwwnnwnn"_s},{u'A',u"wnnnnwnnw"_s},{u'B',u"nnwnnwnnw"_s},
    {u'C',u"wnwnnwnnn"_s},{u'D',u"nnnnwwnnw"_s},{u'E',u"wnnnwwnnn"_s},{u'F',u"nnwnwwnnn"_s},
    {u'G',u"nnnnnwwnw"_s},{u'H',u"wnnnnwwnn"_s},{u'I',u"nnwnnwwnn"_s},{u'J',u"nnnnwwwnn"_s},
    {u'K',u"wnnnnnnww"_s},{u'L',u"nnwnnnnww"_s},{u'M',u"wnwnnnnwn"_s},{u'N',u"nnnnwnnww"_s},
    {u'O',u"wnnnwnnwn"_s},{u'P',u"nnwnwnnwn"_s},{u'Q',u"nnnnnnwww"_s},{u'R',u"wnnnnnwwn"_s},
    {u'S',u"nnwnnnwwn"_s},{u'T',u"nnnnwnwwn"_s},{u'U',u"wwnnnnnnw"_s},{u'V',u"nwwnnnnnw"_s},
    {u'W',u"wwwnnnnnn"_s},{u'X',u"nwnnwnnnw"_s},{u'Y',u"wwnnwnnnn"_s},{u'Z',u"nwwnwnnnn"_s},
    {u'-',u"nwnnnnwnw"_s},{u'.',u"wwnnnnwnn"_s},{u' ',u"nwwnnnwnn"_s},{u'*',u"nwnnwnwnn"_s}
  };
  return patterns.value(c.toUpper());
}

bool barcodeRasterAllowed(qint64 width,qint64 height,const Barcode& barcode,QList<Diagnostic>& diagnostics) {
  if(width>0&&height>0&&width<=32000&&height<=32000&&width*height<=64*1024*1024)return true;
  diagnostics.append({Severity::Error,u"barcode-size"_s,u"Barcode raster dimensions exceed the supported allocation limit."_s,-1,u'^'+barcode.symbology});
  return false;
}

void drawCode39(QImage& image, State& state, const Barcode& barcode, const QString& data, const RenderOptions& options,QList<Diagnostic>& diagnostics) {
  const int narrow=std::max(1,state.barcodeDefaults.moduleWidth);
  if(narrow>32000||!std::isfinite(state.barcodeDefaults.wideToNarrowRatio)
    ||narrow*state.barcodeDefaults.wideToNarrowRatio>32000){
    diagnostics.append({Severity::Error,u"barcode-size"_s,u"Code 39 module dimensions exceed the allocation limit."_s,-1,u"^B3"_s});return;
  }
  const int wide=std::max(narrow+1,qRound(narrow*state.barcodeDefaults.wideToNarrowRatio));
  const int height=std::max(1,barcode.parameters.size()>2 && !barcode.parameters[2].isEmpty() ? barcode.parameters[2].toInt() : state.barcodeDefaults.height);
  const QString encoded=u'*'+data.toUpper()+u'*';
  qint64 width=-qint64(narrow);
  for(const auto c:encoded)for(const auto element:code39Pattern(c))width+=element==u'w'?wide:narrow;
  width+=qint64(encoded.size())*narrow;
  if(width<=0)return;
  if(!barcodeRasterAllowed(width,height,barcode,diagnostics))return;
  QImage symbol(int(width),height,QImage::Format_ARGB32_Premultiplied);symbol.fill(options.background);
  int x=0;
  QPainter painter(&symbol); painter.setBrush(options.foreground); painter.setPen(Qt::NoPen);
  for(const auto c:encoded){
    const auto pattern=code39Pattern(c); if(pattern.isEmpty()) continue;
    for(int i=0;i<pattern.size();++i){const int w=pattern[i]==u'w'?wide:narrow;if(i%2==0)painter.drawRect(x,0,w,height);x+=w;}x+=narrow;
  }
  painter.end();
  QPoint position=state.position;
  if(state.baseline)switch(barcode.orientation){
    case Orientation::Normal:position.ry()-=height;break;
    case Orientation::Rotated90:break;
    case Orientation::Inverted:position.rx()-=int(width);break;
    case Orientation::BottomUp:position-=QPoint(height,int(width));break;
  }
  symbol=rotateImage(symbol,barcode.orientation);
  observeRaster(state,position,symbol);
  QPainter target(&image);target.drawImage(position,symbol);
}

QByteArray decodeDataMatrixData(const QByteArray& input, char marker, bool gs1) {
  QByteArray output;
  for(qsizetype i=0;i<input.size();++i) {
    if(input[i]!=marker||i+1>=input.size()){output.append(input[i]);continue;}
    const char next=input[i+1];
    if(next==marker){output.append(marker);++i;}
    else if(next=='1'){if(!(gs1&&output.isEmpty()))output.append(char(0x1d));++i;}
    else if((next=='d'||next=='D')&&i+4<input.size()&&input[i+2]>='0'&&input[i+2]<='9'&&input[i+3]>='0'&&input[i+3]<='9'&&input[i+4]>='0'&&input[i+4]<='9'){
      output.append(char((input[i+2]-'0')*100+(input[i+3]-'0')*10+input[i+4]-'0'));i+=4;
    } else output.append(input[i]);
  }
  return output;
}

bool validateBarcodeNumbers(const Barcode& barcode,std::initializer_list<int> indices,QList<Diagnostic>& diagnostics) {
  for(const int index:indices){
    if(index>=barcode.parameters.size()||barcode.parameters[index].isEmpty())continue;
    bool ok=false;(void)barcode.parameters[index].toInt(&ok);
    if(!ok){diagnostics.append({Severity::Error,u"barcode-parameter"_s,
      u"Barcode parameter %1 must be an integer."_s.arg(index+1),-1,u'^'+barcode.symbology});return false;}
  }
  return true;
}

void drawDataMatrix(QImage& image,State& state,const Barcode& barcode,const QByteArray& text,const RenderOptions& options,QList<Diagnostic>& diagnostics) {
  if(!validateBarcodeNumbers(barcode,{1,2,3,4,5,7},diagnostics))return;
  if(barcode.parameters.size()>2&&!barcode.parameters[2].isEmpty()&&barcode.parameters[2].toInt()!=200){
    diagnostics.append({Severity::Error,u"datamatrix-quality"_s,u"Only DataMatrix ECC 200 is implemented; no symbol was rendered."_s,-1,u"^BX"_s});return;
  }
  const int module=barcode.parameters.size()>1&&!barcode.parameters[1].isEmpty()?barcode.parameters[1].toInt():3;
  const bool gs1=barcode.parameters.size()>5&&barcode.parameters[5].toInt()==1;
  const QChar escape=barcode.parameters.size()>6&&!barcode.parameters[6].isEmpty()?barcode.parameters[6].front():u'~';
  const int columns=barcode.parameters.size()>3?barcode.parameters[3].toInt():0;
  const int rows=barcode.parameters.size()>4?barcode.parameters[4].toInt():0;
  const int ratio=barcode.parameters.size()>7&&!barcode.parameters[7].isEmpty()?barcode.parameters[7].toInt():1;
  if(ratio!=1&&ratio!=2){diagnostics.append({Severity::Error,u"datamatrix-ratio"_s,u"DataMatrix aspect ratio must be 1 (square) or 2 (rectangular)."_s,-1,u"^BX"_s});return;}
  const bool rectangular=ratio==2;
  if(text.size()>16384){diagnostics.append({Severity::Error,u"datamatrix-encode"_s,u"DataMatrix field exceeds the maximum capacity, including control escapes."_s,-1,u"^BX"_s});return;}
  if(escape.unicode()>255){
    diagnostics.append({Severity::Error,u"datamatrix-encoding"_s,u"DataMatrix requires a single-byte escape character."_s,-1,u"^BX"_s});return;
  }
  const auto decoded=decodeDataMatrixData(text,static_cast<char>(escape.unicode()),gs1);
  const auto lowercase=gs1&&!decoded.contains(char(0x1d))?text.toLower():QByteArray{};
  if(lowercase.contains("\\u001d")||lowercase.contains("\\x1d"))
    diagnostics.append({Severity::Warning,u"gs1-datamatrix-separator"_s,
      u"GS1 DataMatrix field data contains a literal \\\\u001D or \\\\x1D text sequence; use byte 0x1D, |d029, or ^FH hex instead of an escape spelling."_s,-1,u"^BX"_s});
  auto matrix=BarcodeEncoders::dataMatrix(decoded,gs1,rows,columns,rectangular);
  if(!matrix){diagnostics.append({Severity::Error,u"datamatrix-encode"_s,matrix.error(),-1,u"^BX"_s});return;}
  // Validate in 64 bits before multiplying dimensions for QImage.
  if(module<1||module>32000||qint64(matrix->width)*matrix->height*module*module>64*1024*1024){
    diagnostics.append({Severity::Error,u"barcode-size"_s,u"Invalid or unsafe DataMatrix module size."_s,-1,u"^BX"_s});return;
  }
  QImage symbol(matrix->width*module,matrix->height*module,QImage::Format_ARGB32_Premultiplied);symbol.fill(options.background);
  QPainter p(&symbol);p.setPen(Qt::NoPen);p.setBrush(options.foreground);
  for(int y=0;y<matrix->height;++y)for(int x=0;x<matrix->width;++x)if(matrix->at(x,y))p.drawRect(x*module,y*module,module,module);
  p.end();placeNewSymbol(image,symbol,state,barcode.orientation);
}

int qrMagnification(const Barcode& barcode) {
  bool valid=false;
  const int requested=barcode.parameters.size()>2?barcode.parameters[2].toInt(&valid):0;
  return valid&&requested>0?std::min(requested,100):2;
}

void drawQrCode(QImage& image,State& state,const Barcode& barcode,const QByteArray& fieldData,const RenderOptions& options,QList<Diagnostic>& diagnostics) {
  const int model=barcode.parameters.size()>1&&!barcode.parameters[1].isEmpty()
    ? barcode.parameters[1].toInt() : 2;
  if(model!=2){
    diagnostics.append({Severity::Error,u"qrcode-model"_s,
      u"Only QR Model 2 is implemented; no symbol was rendered."_s,-1,u"^BQ"_s});
    return;
  }
  QChar errorCorrection=u'M';
  QByteArray data=fieldData;
  if(data.size()>=3&&data[2]==','){
    errorCorrection=QChar::fromLatin1(data[0]).toUpper();
    const QChar inputMode=QChar::fromLatin1(data[1]).toUpper();
    data.remove(0,3);
    if(inputMode==u'M'&&!data.isEmpty()){
      const QChar characterMode=QChar::fromLatin1(data.front()).toUpper();
      if(characterMode==u'N'||characterMode==u'A'||characterMode==u'K'){
        data.remove(0,1);
        // Zebra manual alphanumeric mode uses the QR alphanumeric alphabet;
        // lower-case input is normalized to upper case by the printer.
        if(characterMode==u'A'){
          data=data.toUpper();
          data.removeIf([](char value){
            return !QStringView{u"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:"}.contains(QChar::fromLatin1(value));
          });
        }
      }
      else if(characterMode==u'B'&&data.size()>=5){
        bool ok=false;const int bytes=data.mid(1,4).toInt(&ok);
        if(ok&&bytes<=data.size()-5)data=data.mid(5,bytes);
      }
    }
  }
  const int module=qrMagnification(barcode);
  auto matrix=BarcodeEncoders::qrCode(data,errorCorrection);
  if(!matrix){diagnostics.append({Severity::Error,u"qrcode-encode"_s,matrix.error(),-1,u"^BQ"_s});return;}
  if(!barcodeRasterAllowed(qint64(matrix->width)*module,qint64(matrix->height)*module,barcode,diagnostics))return;
  QImage symbol(matrix->width*module,matrix->height*module,QImage::Format_ARGB32_Premultiplied);symbol.fill(options.background);
  QPainter painter(&symbol);painter.setPen(Qt::NoPen);painter.setBrush(options.foreground);
  for(int y=0;y<matrix->height;++y)for(int x=0;x<matrix->width;++x)if(matrix->at(x,y))painter.drawRect(x*module,y*module,module,module);
  painter.end();
  State placed=state;
  if(state.baseline)placed.position.ry()-=3*module;
  else placed.position.ry()+=std::max(0,state.barcodeDefaults.height);
  // ^BQ defines only normal orientation; ^FW and the orientation slot do not
  // rotate QR. Its typeset anchor also includes the lower quiet-zone margin.
  placeNewSymbol(image,symbol,placed,Orientation::Normal);
}

void drawEan13(QImage& image,State& state,const Barcode& barcode,const QString& text,const RenderOptions& options,QList<Diagnostic>& diagnostics) {
  QString normalized;auto modules=BarcodeEncoders::ean13(text,&normalized);
  if(!modules){diagnostics.append({Severity::Error,u"ean13-encode"_s,modules.error(),-1,u"^BE"_s});return;}
  const int module=std::max(1,state.barcodeDefaults.moduleWidth);
  if(module>32000){diagnostics.append({Severity::Error,u"barcode-size"_s,u"EAN-13 module dimensions exceed the allocation limit."_s,-1,u"^BE"_s});return;}
  int height=state.barcodeDefaults.height>0?state.barcodeDefaults.height:100;
  if(barcode.parameters.size()>1&&!barcode.parameters[1].isEmpty())height=std::max(1,barcode.parameters[1].toInt());
  const bool interpretation=barcode.parameters.size()<=2||barcode.parameters[2].compare(u"N",Qt::CaseInsensitive)!=0;
  const bool interpretationAbove=barcode.parameters.size()>3&&barcode.parameters[3].compare(u"Y",Qt::CaseInsensitive)==0;
  const int guardExtra=interpretation?std::max(1,5*module-2):13;
  const int textHeight=interpretation?std::max(12,12*module):0;
  const int leftPad=interpretation?14*module:0;
  const int barsTop=interpretation&&interpretationAbove?textHeight:0;
  if(!barcodeRasterAllowed(qint64(leftPad)+95LL*module,qint64(height)+guardExtra+textHeight,barcode,diagnostics))return;
  QImage symbol(leftPad+95*module,height+guardExtra+textHeight,QImage::Format_ARGB32_Premultiplied);symbol.fill(options.background);
  QPainter p(&symbol);p.setPen(Qt::NoPen);p.setBrush(options.foreground);
  for(int i=0;i<modules->size();++i)if((*modules)[i]){const bool guard=i<3||(i>=45&&i<=49)||i>=92;p.drawRect(leftPad+i*module,barsTop,module,height+(guard?guardExtra:0));}
  if(interpretation){
    // EAN/UPC human-readable interpretation uses OCR-B rather than Zebra Font 0.
    p.setFont(eanInterpretationFont(std::max(12,12*module)));
    p.setPen(options.foreground);
    if(interpretationAbove){
      const int textTop=-(3*module-1);
      p.drawText(QRect(leftPad,textTop,95*module,textHeight),Qt::AlignCenter,normalized);
    }else{
      const int textTop=height-(2*module-1);
      p.drawText(QRect(-module,textTop,leftPad,textHeight),Qt::AlignCenter,normalized.left(1));
      p.drawText(QRect(leftPad+3*module,textTop,42*module,textHeight),Qt::AlignCenter,normalized.mid(1,6));
      p.drawText(QRect(leftPad+50*module,textTop,42*module,textHeight),Qt::AlignCenter,normalized.mid(7,6));
    }
  }
  p.end();
  const QSize sourceSize=symbol.size();
  symbol=rotateImage(symbol,barcode.orientation);
  QPoint position=state.position;
  if(state.baseline){
    // ^FT fixes the first bar's lower edge, excluding guard extensions and
    // interpretation. Rotate this geometric edge (not a pixel index).
    const QPoint anchor(leftPad,barsTop+height);
    switch(barcode.orientation){
      case Orientation::Normal:position-=anchor;break;
      case Orientation::Rotated90:position-=QPoint(sourceSize.height()-anchor.y(),anchor.x());break;
      case Orientation::Inverted:position-=QPoint(sourceSize.width()-anchor.x(),sourceSize.height()-anchor.y());break;
      case Orientation::BottomUp:position-=QPoint(anchor.y(),sourceSize.width()-anchor.x());break;
    }
  }else switch(barcode.orientation){
    case Orientation::Normal:
      position.rx()-=leftPad;
      if(interpretationAbove)position.ry()-=textHeight;
      break;
    case Orientation::Rotated90:
      position.rx()-=textHeight+guardExtra;
      position.ry()-=leftPad;
      break;
    case Orientation::Inverted:
      position.ry()-=textHeight+guardExtra;
      break;
    case Orientation::BottomUp:
      break;
  }
  observeRaster(state,position,symbol);
  QPainter target(&image);target.drawImage(position,symbol);
}

QString code128Interpretation(QStringView data) {
  QString result;result.reserve(data.size());
  for(qsizetype i=0;i<data.size();++i){
    if(data[i]==u'>'&&i+1<data.size()&&QStringView{u"<0=123456789:;"}.contains(data[i+1])){
      if(data[i+1]==u'<')result.append(u'>');
      ++i;continue;
    }
    result.append(data[i]);
  }
  return result;
}

void drawCode128(QImage& image,State& state,const Barcode& barcode,const QString& text,const RenderOptions& options,QList<Diagnostic>& diagnostics,RenderFonts& fonts) {
  if(text.size()>4096){diagnostics.append({Severity::Error,u"code128-encode"_s,u"Code 128 rendered field data exceeds 4096 characters."_s,state.barcodeOffset,state.barcodeSource});return;}
  const auto rasterAllowed=[&](qint64 width,qint64 height){
    if(width>0&&height>0&&width<=32000&&height<=32000&&width*height<=64*1024*1024)return true;
    diagnostics.append({Severity::Error,u"barcode-dimensions"_s,u"Code 128 raster exceeds the supported dimensions or 64 megapixels."_s,state.barcodeOffset,state.barcodeSource});return false;
  };
  const QChar mode=barcode.parameters.size()>5&&!barcode.parameters[5].isEmpty()?barcode.parameters[5].front():u'N';
  const bool uccCheck=barcode.parameters.size()>4&&barcode.parameters[4].compare(u"Y",Qt::CaseInsensitive)==0;
  auto modules=BarcodeEncoders::code128(text,mode,uccCheck);
  if(!modules){diagnostics.append({Severity::Error,u"code128-encode"_s,modules.error(),-1,u"^BC"_s});return;}
  const int module=std::clamp(state.barcodeDefaults.moduleWidth,1,10);
  int height=state.barcodeDefaults.height>0?state.barcodeDefaults.height:100;
  if(barcode.parameters.size()>1&&!barcode.parameters[1].isEmpty())height=std::max(1,barcode.parameters[1].toInt());
  if(!rasterAllowed(qint64(modules->size())*module,height))return;
  const bool interpretation=barcode.parameters.size()<=2||barcode.parameters[2].compare(u"N",Qt::CaseInsensitive)!=0;
  const bool interpretationAbove=barcode.parameters.size()>3&&!barcode.parameters[3].isEmpty()
    ?barcode.parameters[3].compare(u"Y",Qt::CaseInsensitive)==0:mode.toUpper()==u'U';
  if(interpretation&&!state.barcodeInterpretationFont){
    const auto captionData=BarcodeEncoders::Detail::code128UccData(text,mode,uccCheck);
    const QString captionText=code128Interpretation(captionData?*captionData:text);
    const qreal fontSize=module*9.0;
    const qreal advance=fontSize*1365.0/2048;
    const int barsWidth=int(modules->size())*module;
    const int baseline=interpretationAbove?-(2*module+8):height+7*module+6;
    const int turns=barcode.orientation==Orientation::Rotated90?1:barcode.orientation==Orientation::Inverted?2:barcode.orientation==Orientation::BottomUp?3:0;
    qreal cursor=(barsWidth-advance*captionText.size())/2;
    struct PositionedGlyph {PrinterGlyph glyph;QPoint baseline;};
    QList<PositionedGlyph> glyphs;
    QRect bounds(0,0,barsWidth,height);
    for(const QChar character:captionText){
      const auto glyph=fonts.fontARaster().glyph(PrinterFontMetrics::fontACharacter(character.unicode()),int(fontSize),int(fontSize),turns);
      if(!glyph){diagnostics.append({Severity::Error,u"font-raster"_s,glyph.error(),state.barcodeOffset,state.barcodeSource});return;}
      // Round the physical position after the field rotation. A centered
      // caption can land on half-dots; reflecting a rounded normal position
      // selects the opposite tie at 180 and 270 degrees.
      const int glyphX=turns>=2?barsWidth-qRound(barsWidth-cursor):qRound(cursor);
      const QPoint position(glyphX,baseline);
      bounds|=QRect(position+glyph->bearing,glyph->alpha.size());
      glyphs.append({*glyph,position});
      cursor+=advance;
    }
    if(!rasterAllowed(bounds.width(),bounds.height()))return;
    QImage symbol(bounds.size(),QImage::Format_ARGB32_Premultiplied);symbol.fill(options.background);
    QPainter painter(&symbol);painter.setRenderHint(QPainter::Antialiasing,false);painter.setPen(Qt::NoPen);painter.setBrush(options.foreground);
    for(qsizetype i=0;i<modules->size();++i)
      if((*modules)[i])painter.drawRect(int(i)*module-bounds.left(),-bounds.top(),module,height);
    for(const auto& glyph:glyphs)paintPrinterGlyph(painter,glyph.baseline-bounds.topLeft(),glyph.glyph,options.foreground);
    painter.end();
    QPoint position=state.position;
    if(state.baseline)position-=rotatedEdge(QPoint(-bounds.left(),height-bounds.top()),symbol.size(),barcode.orientation);
    else switch(barcode.orientation){
      case Orientation::Normal:position+=bounds.topLeft();break;
      case Orientation::Rotated90:position+=QPoint(height-bounds.bottom()-1,bounds.left());break;
      case Orientation::Inverted:position+=QPoint(barsWidth-bounds.right()-1,height-bounds.bottom()-1);break;
      case Orientation::BottomUp:position+=QPoint(bounds.top(),barsWidth-bounds.right()-1);break;
    }
    const auto rotated=rotateImage(symbol,barcode.orientation);
    observeRaster(state,position,rotated);
    QPainter target(&image);target.drawImage(position,rotated);
    return;
  }
  const int gap=interpretation?5:0;
  QImage caption;
  if(interpretation){
    const auto captionData=BarcodeEncoders::Detail::code128UccData(text,mode,uccCheck);
    const QString interpretationText=code128Interpretation(captionData?*captionData:text);
    // The default interpretation scales with ^BY's module width. An ^A in
    // this field may override it; an ^A belonging to a previous ^FS field may
    // not leak into the barcode caption.
    const auto captionFont=state.barcodeInterpretationFont;
    if(captionFont&&(captionFont->height>32000||captionFont->width>32000)){rasterAllowed(32001,1);return;}
    QImage field;
    if(captionFont&&captionFont->font==u'0'){
      const Font0Metrics metrics(std::max(1,captionFont->height),captionFont->width,fonts);
      const auto outline=metrics.outline(interpretationText);
      const QRect bounds=outline.boundingRect().toAlignedRect();
      if(!rasterAllowed(std::max(1,bounds.width()),std::max(1,bounds.height())))return;
      field=QImage(std::max(1,bounds.width()),std::max(1,bounds.height()),QImage::Format_ARGB32_Premultiplied);
      field.fill(Qt::transparent);
      QPainter painter(&field);painter.setRenderHint(QPainter::Antialiasing,false);
      painter.fillPath(outline.translated(-bounds.left(),-bounds.top()),options.foreground);
    }else{
      const QFont font=captionFont
        ? builtInFont(captionFont->font,captionFont->height,captionFont->width)
        : builtInFont(u'A',module*10);
      const QRect bounds=QFontMetrics(font).boundingRect(interpretationText).adjusted(-1,-1,1,1);
      if(!rasterAllowed(std::max(1,bounds.width()),std::max(1,bounds.height())))return;
      field=QImage(std::max(1,bounds.width()),std::max(1,bounds.height()),QImage::Format_ARGB32_Premultiplied);
      field.fill(Qt::transparent);
      QPainter painter(&field);painter.setRenderHint(QPainter::TextAntialiasing,false);
      painter.setFont(font);painter.setPen(options.foreground);
      painter.drawText(-bounds.left(),-bounds.top(),interpretationText);
    }
    const QRect ink=alphaBounds(field);
    if(!ink.isEmpty())caption=field.copy(ink);
  }
  const int captionHeight=caption.height();
  const int barsTop=interpretationAbove?captionHeight+gap:0;
  const int symbolHeight=height+captionHeight+gap;
  if(!rasterAllowed(qint64(modules->size())*module,symbolHeight))return;
  QImage symbol(modules->size()*module,symbolHeight,QImage::Format_ARGB32_Premultiplied);symbol.fill(options.background);
  QPainter painter(&symbol);painter.setRenderHint(QPainter::Antialiasing,false);painter.setPen(Qt::NoPen);painter.setBrush(options.foreground);
  for(qsizetype i=0;i<modules->size();++i)if((*modules)[i])painter.drawRect(static_cast<int>(i)*module,barsTop,module,height);
  if(!caption.isNull()){
    const int textTop=interpretationAbove?0:height+gap;
    const int textLeft=state.barcodeInterpretationFont?0:(symbol.width()-caption.width())/2;
    painter.drawImage(textLeft,textTop,caption);
  }
  painter.end();
  const QSize sourceSize=symbol.size();
  symbol=rotateImage(symbol,barcode.orientation);
  QPoint position=state.position;
  if(state.baseline){
    const int base=barsTop+height;
    switch(barcode.orientation){
      case Orientation::Normal:position.ry()-=base;break;
      case Orientation::Rotated90:position.rx()-=sourceSize.height()-base;break;
      case Orientation::Inverted:position-=QPoint(sourceSize.width(),sourceSize.height()-base);break;
      case Orientation::BottomUp:position-=QPoint(base,sourceSize.width());break;
    }
  }else if(barcode.orientation==Orientation::Normal)position.ry()-=barsTop;
  observeRaster(state,position,symbol);
  QPainter target(&image);target.drawImage(position,symbol);
}

void drawCodabar(QImage& image,State& state,const Barcode& barcode,const QString& text,const RenderOptions& options,QList<Diagnostic>& diagnostics) {
  const int ratio=std::clamp(qRound(state.barcodeDefaults.wideToNarrowRatio),2,3);
  auto modules=BarcodeEncoders::codabar(text,ratio);
  if(!modules){diagnostics.append({Severity::Error,u"codabar-encode"_s,modules.error(),-1,u"^BK"_s});return;}
  const int module=std::clamp(state.barcodeDefaults.moduleWidth,1,10);
  const int height=barcode.parameters.size()>2&&!barcode.parameters[2].isEmpty()?std::max(1,barcode.parameters[2].toInt()):std::max(1,state.barcodeDefaults.height);
  if(!barcodeRasterAllowed(qint64(modules->size())*module,height,barcode,diagnostics))return;
  QImage symbol(modules->size()*module,height,QImage::Format_ARGB32_Premultiplied);symbol.fill(options.background);
  QPainter painter(&symbol);painter.setPen(Qt::NoPen);painter.setBrush(options.foreground);
  for(qsizetype i=0;i<modules->size();++i)if((*modules)[i])painter.drawRect(static_cast<int>(i)*module,0,module,height);
  painter.end();placeNewSymbol(image,symbol,state,barcode.orientation);
}

void drawPdf417(QImage& image,State& state,const Barcode& barcode,const QByteArray& text,const RenderOptions& options,QList<Diagnostic>& diagnostics) {
  if(!validateBarcodeNumbers(barcode,{1,2,3,4},diagnostics))return;
  const int module=std::clamp(state.barcodeDefaults.moduleWidth,1,100);
  const int rowHeight=barcode.parameters.size()>1&&!barcode.parameters[1].isEmpty()?barcode.parameters[1].toInt():module*3;
  const int security=barcode.parameters.size()>2&&!barcode.parameters[2].isEmpty()?barcode.parameters[2].toInt():0;
  const int columns=barcode.parameters.size()>3&&!barcode.parameters[3].isEmpty()?barcode.parameters[3].toInt():0;
  const int rows=barcode.parameters.size()>4&&!barcode.parameters[4].isEmpty()?barcode.parameters[4].toInt():0;
  const bool truncated=barcode.parameters.size()>5&&barcode.parameters[5].compare(u"Y",Qt::CaseInsensitive)==0;
  if(barcode.parameters.size()>5&&!barcode.parameters[5].isEmpty()&&!truncated&&barcode.parameters[5].compare(u"N",Qt::CaseInsensitive)!=0){
    diagnostics.append({Severity::Error,u"barcode-parameter"_s,u"PDF417 truncation must be Y or N."_s,-1,u"^B7"_s});return;
  }
  if(text.size()>2710){diagnostics.append({Severity::Error,u"pdf417-encode"_s,u"PDF417 data exceeds the maximum capacity."_s,-1,u"^B7"_s});return;}
  auto symbolData=BarcodeEncoders::Pdf417::encode(text,security,columns,rows,truncated);
  if(!symbolData){diagnostics.append({Severity::Error,u"pdf417-encode"_s,symbolData.error(),-1,u"^B7"_s});return;}
  const auto& matrix=symbolData->matrix;
  if(rowHeight<1||qint64(matrix.width)*module*matrix.height*rowHeight>64*1024*1024){
    diagnostics.append({Severity::Error,u"barcode-size"_s,u"Invalid or unsafe PDF417 row height."_s,-1,u"^B7"_s});return;
  }
  QImage symbol(matrix.width*module,matrix.height*rowHeight,QImage::Format_ARGB32_Premultiplied);symbol.fill(options.background);
  QPainter painter(&symbol);painter.setPen(Qt::NoPen);painter.setBrush(options.foreground);
  for(int y=0;y<matrix.height;++y)for(int x=0;x<matrix.width;++x)if(matrix.at(x,y))painter.drawRect(x*module,y*rowHeight,module,rowHeight);
  painter.end();placeNewSymbol(image,symbol,state,barcode.orientation);
}

// MaxiCode geometry follows OkapiBarcode's MaxiCode/Hexagon plot elements.
// Copyright 2014-2015 Robin Stuart, Daniel Gredler and Robert Elliott.
// Apache-2.0; see third_party/okapi-maxicode/NOTICE and LICENSE.
// Raster conversion is native, deterministic and expressed in printer dots.
void fillMaxiCodePolygon(QImage& image,const QVector<QPointF>& points,QRgb colour,bool subpixel=false) {
  double minimumY=points.front().y(),maximumY=minimumY;
  for(const auto& point:points){minimumY=std::min(minimumY,point.y());maximumY=std::max(maximumY,point.y());}
  const int firstY=std::max(0,static_cast<int>(std::ceil(minimumY)));
  const int lastY=std::min(image.height(),static_cast<int>(std::ceil(maximumY)));
  for(int y=firstY;y<lastY;++y){
    double left=std::numeric_limits<double>::max(),right=std::numeric_limits<double>::lowest();
    for(qsizetype i=0;i<points.size();++i){
      const auto& a=points[i];const auto& b=points[(i+1)%points.size()];
      if(y<std::min(a.y(),b.y())||y>=std::max(a.y(),b.y()))continue;
      const double x=a.x()+(b.x()-a.x())*(y-a.y())/(b.y()-a.y());
      left=std::min(left,x);right=std::max(right,x);
    }
    if(left>right)continue;
    // The finder uses 8-bit subpixel scanline boundaries. Hexagon vertices
    // have already been snapped to integer dots, before scan conversion.
    if(subpixel){left=std::floor(left*256.0)/256.0;right=std::floor(right*256.0)/256.0;}
    const int firstX=std::max(0,static_cast<int>(std::ceil(left)));
    const int lastX=std::min(image.width(),static_cast<int>(std::ceil(right)));
    auto* scanline=reinterpret_cast<QRgb*>(image.scanLine(y));
    std::fill(scanline+firstX,scanline+lastX,colour);
  }
}

QVector<QPointF> maxiCodeFinderCircle(double centerX,double centerY,double radius) {
  // Standard four-cubic circle, eight line segments per quadrant at this
  // fixed dot size. Sampling the analytic circle changes boundary pixels.
  constexpr double control=0.5522847498307933;
  QVector<QPointF> points;points.reserve(32);
  for(int quadrant=0;quadrant<4;++quadrant)for(int step=0;step<8;++step){
    const double t=step/8.0,u=1.0-t;
    double x=u*u*u+3.0*u*u*t+3.0*u*t*t*control;
    double y=3.0*u*u*t*control+3.0*u*t*t+t*t*t;
    for(int turn=0;turn<quadrant;++turn){const double previousX=x;x=-y;y=previousX;}
    points.append(QPointF(centerX+radius*x,centerY+radius*y));
  }
  return points;
}

QImage rasterizeMaxiCode(const BarcodeEncoders::Matrix& grid,const RenderOptions& options) {
  // The fixed MaxiCode pitch is independent of display/printer DPI, matching
  // the 203-DPI dot geometry used by Labelary's ^BD command at every density.
  constexpr double scale=2.72;
  constexpr int width=static_cast<int>(74*scale),height=static_cast<int>(72*scale);
  QImage symbol(width,height,QImage::Format_ARGB32_Premultiplied);symbol.fill(options.background);
  const QRgb ink=qPremultiply(options.foreground.rgba()),paper=qPremultiply(options.background.rgba());
  constexpr double offsetX[]{0.0,0.86,0.86,0.0,-0.86,-0.86};
  constexpr double offsetY[]{1.0,0.5,-0.5,-1.0,-0.5,0.5};
  QVector<QPointF> points;points.reserve(6);
  for(int row=0;row<33;++row)for(int column=0;column<30-(row&1);++column){
    if(!grid.at(column,row))continue;
    const double centerX=(2.46*column+1.23+((row&1)?1.23:0.0))*scale;
    const double centerY=(2.135*row+1.43)*scale;
    points.clear();
    for(int vertex=0;vertex<6;++vertex)
      points.append(QPointF(static_cast<int>(centerX+offsetX[vertex]*1.25*scale),
                           static_cast<int>(centerY+offsetY[vertex]*1.25*scale)));
    fillMaxiCodePolygon(symbol,points,ink);
  }
  constexpr double radii[]{10.85,8.97,7.10,5.22,3.31,1.43};
  for(int ring=0;ring<6;++ring)
    fillMaxiCodePolygon(symbol,maxiCodeFinderCircle(35.76*scale,35.60*scale,radii[ring]*scale),
                        (ring&1)?paper:ink,true);
  return symbol;
}

void drawMaxiCode(QImage& image,State& state,const Barcode& barcode,const QByteArray& text,const RenderOptions& options,QList<Diagnostic>& diagnostics) {
  const int mode=barcode.parameters.isEmpty()||barcode.parameters[0].isEmpty()?2:barcode.parameters[0].toInt();
  const int positionIndex=barcode.parameters.size()>1&&!barcode.parameters[1].isEmpty()?barcode.parameters[1].toInt():1;
  const int total=barcode.parameters.size()>2&&!barcode.parameters[2].isEmpty()?barcode.parameters[2].toInt():1;
  auto encoded=BarcodeEncoders::MaxiCode::encode(text,mode,positionIndex,total);
  if(!encoded){diagnostics.append({Severity::Error,u"maxicode-encode"_s,encoded.error(),-1,u"^BD"_s});return;}
  QImage symbol=rotateImage(rasterizeMaxiCode(encoded->grid,options),barcode.orientation);
  QPoint position=state.position;if(state.baseline)position.ry()-=symbol.height();
  observeRaster(state,position,symbol);
  QPainter target(&image);target.drawImage(position,symbol);
}

int barcodeParameter(const Barcode& barcode,qsizetype index,int fallback) {
  if(index>=barcode.parameters.size()||barcode.parameters[index].isEmpty())return fallback;
  bool ok=false;const int value=barcode.parameters[index].toInt(&ok);return ok?value:fallback;
}

void placeNewSymbol(QImage& image,const QImage& original,const State& state,Orientation orientation) {
  QPoint position=state.position;
  if(state.baseline)position-=rotatedEdge(QPoint(state.justification==Justification::Right?original.width():0,original.height()),original.size(),orientation);
  else if(state.justification==Justification::Right)position.rx()-=(orientation==Orientation::Rotated90||orientation==Orientation::BottomUp?original.height():original.width());
  const auto rotated=rotateImage(original,orientation);
  observeRaster(state,position,rotated);
  QPainter painter(&image);painter.drawImage(position,rotated);
}

void drawExtendedLinear(QImage& image,State& state,const Barcode& barcode,const QString& data,const RenderOptions& options,QList<Diagnostic>& diagnostics,RenderFonts& fonts) {
  const int module=std::clamp(state.barcodeDefaults.moduleWidth,1,10);
  const int height=barcodeParameter(barcode,1,std::max(1,state.barcodeDefaults.height));
  if(height<1||height>32000){diagnostics.append({Severity::Error,u"barcode-height"_s,u"Barcode height must be 1 through 32000 dots."_s,-1,u'^'+barcode.symbology});return;}
  const bool interleaved=barcode.symbology==u"B2",industrial=barcode.symbology==u"BI";
  const bool retail=barcode.symbology==u"B8"||barcode.symbology==u"B9"||barcode.symbology==u"BU";
  const bool caption=barcode.parameters.size()<=2||barcode.parameters[2]!=u"N";
  const bool above=barcode.parameters.size()>3&&barcode.parameters[3]==u"Y";
  const bool check=barcode.parameters.size()>4&&barcode.parameters[4]==u"Y";
  if(!std::isfinite(state.barcodeDefaults.wideToNarrowRatio)||state.barcodeDefaults.wideToNarrowRatio<2.0||state.barcodeDefaults.wideToNarrowRatio>3.0){
    diagnostics.append({Severity::Error,u"barcode-ratio"_s,u"Barcode wide-to-narrow ratio must be finite and between 2 and 3."_s,-1,u'^'+barcode.symbology});return;
  }
  const int wide=std::clamp(static_cast<int>(module*state.barcodeDefaults.wideToNarrowRatio),module,30);
  auto encoded=BarcodeEncoders::Linear::encode(barcode.symbology,data,module,wide,interleaved&&check);
  if(!encoded){diagnostics.append({Severity::Error,u"barcode-encode"_s,encoded.error(),-1,u'^'+barcode.symbology});return;}
  const int scale=interleaved||industrial?1:module;
  const int barsWidth=static_cast<int>(encoded->modules.size())*scale;
  const int guardExtra=retail?13:0;
  const int textHeight=caption?12*module:0;
  const int sidePad=caption&&retail&&barcode.symbology!=u"B8"?14*module:0;
  const int barsTop=caption&&above?textHeight:0;
  if(qint64(barsWidth+2*sidePad)*qint64(height+guardExtra+textHeight)>64*1024*1024){diagnostics.append({Severity::Error,u"barcode-size"_s,u"Barcode raster exceeds the allocation limit."_s,-1,u'^'+barcode.symbology});return;}
  QImage symbol(barsWidth+2*sidePad,height+guardExtra+textHeight,QImage::Format_ARGB32_Premultiplied);symbol.fill(Qt::transparent);
  QPainter painter(&symbol);painter.setPen(Qt::NoPen);painter.setBrush(options.foreground);
  for(qsizetype i=0;i<encoded->modules.size();++i)if(encoded->modules[i]){
    const int count=static_cast<int>(encoded->modules.size());
    const int outsideGuard=barcode.symbology==u"BU"?10:3;
    const bool guard=retail&&(i<outsideGuard||i>=count-(barcode.symbology==u"B9"?6:outsideGuard)||(barcode.symbology!=u"B9"&&i>=count/2-2&&i<=count/2+2));
    painter.drawRect(sidePad+static_cast<int>(i)*scale,barsTop,scale,height+(guard?guardExtra:0));
  }
  if(caption){
    const QString text=encoded->text;
    if(retail&&!above){
      const bool fontA=module<3;
      const int fontScale=std::max(1,module/4);
      const int cellWidth=fontA?6*module:20*fontScale;
      const int baseline=height+(fontA?7*module+4:23*fontScale+4);
      const auto print=[&](int origin,QStringView part,bool grouped=true){
        const int count=static_cast<int>(part.size());
        const qreal pitch=grouped&&count>1?qreal((7*count-3)*module-cellWidth)/(count-1):cellWidth;
        for(int character=0;character<count;++character){
          const int x=sidePad+qRound(origin+character*pitch);
          if(fontA){
            paintPrinterFontAGlyph(painter,QPoint(x,baseline),part[character],9*module,5*module,options.foreground,fonts,barcode.orientation);
          }else{
            const auto& glyph=RetailFont::digits[fontScale-1][part[character].unicode()-u'0'];
            for(int row=0;row<glyph.height;++row){
              const auto bits=RetailFont::rowBits(fontScale-1,part[character].unicode()-u'0',static_cast<char>(barcode.orientation),row);
              for(int column=0;column<glyph.width;){
                if(!(bits&(std::uint32_t{1}<<column))){++column;continue;}
                const int start=column;
                while(column<glyph.width&&(bits&(std::uint32_t{1}<<column)))++column;
                painter.fillRect(x+glyph.xOffset+start,baseline+glyph.yOffset+row,column-start,1,options.foreground);
              }
            }
          }
        }
      };
      if(barcode.symbology==u"B8"){
        print(5*module,QStringView{text}.first(4));print(37*module,QStringView{text}.last(4));
      }else{
        print(-5*module-cellWidth,QStringView{text}.first(1),false);
        if(barcode.symbology==u"B9")print(5*module,QStringView{text}.mid(1,6));
        else{print(12*module,QStringView{text}.mid(1,5));print(51*module,QStringView{text}.mid(6,5));}
        if(barcode.parameters.size()<=4||barcode.parameters[4]!=u"N")print(barsWidth+2*module,QStringView{text}.last(1),false);
      }
    }else{
      QString interpretation=text;
      if(barcode.symbology==u"BA"){if(check)interpretation+=encoded->checkText;interpretation=QChar(0x25af)+interpretation+QChar(0x25af);}
      const qreal advance=qreal(9*module)*1365/2048;
      const qreal first=sidePad+(barsWidth-(retail?module:0)-advance*interpretation.size())/2.0;
      const int baseline=above?barsTop-5*module+1:height+9*module;
      for(qsizetype character=0;character<interpretation.size();++character){
        const qreal origin=first+character*advance;
        // A half-dot origin rounds in the direction of the transformed axis.
        const int x=barcode.orientation==Orientation::Inverted||barcode.orientation==Orientation::BottomUp
          ?static_cast<int>(std::ceil(origin-0.5)):qRound(origin);
        paintPrinterFontAGlyph(painter,QPoint(x,baseline),interpretation[character],9*module,5*module,options.foreground,fonts,barcode.orientation);
      }
    }
  }
  painter.end();
  State placed=state;
  if(state.baseline){
    // ^FT anchors the lower edge of the first bar, excluding the caption
    // and the retail guard extensions, before applying the orientation.
    const QPoint anchor(sidePad+(state.justification==Justification::Right?barsWidth:0),barsTop+height);
    placed.position-=rotatedEdge(anchor,symbol.size(),barcode.orientation);
    placed.baseline=false;
    placed.justification=Justification::Left;
  }else{
    // ^FO names the top-left corner of the rotated bar body. Caption space
    // can occur on either side after rotation, but must not move that body.
    switch(barcode.orientation){
      case Orientation::Normal:placed.position-=QPoint(sidePad,barsTop);break;
      case Orientation::Rotated90:placed.position-=QPoint(symbol.height()-barsTop-height,sidePad);break;
      case Orientation::Inverted:placed.position-=QPoint(sidePad,symbol.height()-barsTop-height);break;
      case Orientation::BottomUp:placed.position-=QPoint(barsTop,sidePad);break;
    }
  }
  placeNewSymbol(image,symbol,placed,barcode.orientation);
}

void drawExtendedMatrix(QImage& image,State& state,const Barcode& barcode,const FieldData& field,const RenderOptions& options,QList<Diagnostic>& diagnostics) {
  using BarcodeEncoders::Matrix;
  std::expected<Matrix,QString> matrix=std::unexpected(u"Unsupported matrix barcode options"_s);
  int module=std::clamp(state.barcodeDefaults.moduleWidth,1,10),rowHeight=module;
  const auto& data=field.data;
  if(barcode.symbology==u"BR"&&std::ranges::any_of(data,[](QChar c){return c.unicode()>255;})){
    diagnostics.append({Severity::Error,u"barcode-encoding"_s,u"This barcode requires explicit byte data; use ^FH for binary fields."_s,-1,u'^'+barcode.symbology});return;
  }
  if(barcode.symbology==u"BF"){
    if(!validateBarcodeNumbers(barcode,{1,2},diagnostics))return;
    matrix=BarcodeEncoders::MicroPdf417::encode(*field.bytes,barcodeParameter(barcode,2,0));
    rowHeight=barcodeParameter(barcode,1,matrix?std::max(1,state.barcodeDefaults.height/matrix->height):1);
  }else if(barcode.symbology==u"BR"){
    if(!validateBarcodeNumbers(barcode,{1,2,3,4,5},diagnostics))return;
    const int separator=barcodeParameter(barcode,3,1),height=barcodeParameter(barcode,4,25);
    if(separator<1||separator>2||height<1||height>32000){
      diagnostics.append({Severity::Error,u"barcode-parameter"_s,u"GS1 DataBar requires separator height 1 or 2 and bar height 1 through 32000."_s,-1,u"^BR"_s});return;
    }
    module=barcodeParameter(barcode,2,2);rowHeight=module;
    matrix=BarcodeEncoders::DataBar::encode(data,barcodeParameter(barcode,1,1),barcodeParameter(barcode,4,25),barcodeParameter(barcode,5,0));
  }else{
    if(!validateBarcodeNumbers(barcode,{1,3,5},diagnostics))return;
    for(const int index:{2,4})if(index<barcode.parameters.size()&&!barcode.parameters[index].isEmpty()
      &&barcode.parameters[index]!=u"Y"&&barcode.parameters[index]!=u"N"){
      diagnostics.append({Severity::Error,u"barcode-parameter"_s,u"Aztec ECI and reader initialization flags must be Y or N."_s,-1,u'^'+barcode.symbology});return;
    }
    module=barcodeParameter(barcode,1,2);rowHeight=module;
    const bool eci=barcode.parameters.size()>2&&barcode.parameters[2]==u"Y";
    if(eci||barcodeParameter(barcode,5,1)!=1){matrix=std::unexpected(u"Aztec embedded ECI and structured append are not supported."_s);}
    else matrix=BarcodeEncoders::Aztec::encode(*field.bytes,barcodeParameter(barcode,3,23),barcode.parameters.size()>4&&barcode.parameters[4]==u"Y");
  }
  if(!matrix){diagnostics.append({Severity::Error,u"barcode-encode"_s,matrix.error(),-1,u'^'+barcode.symbology});return;}
  if(module<1||module>10||rowHeight<1||rowHeight>9999||qint64(matrix->width)*module*matrix->height*rowHeight>64*1024*1024){diagnostics.append({Severity::Error,u"barcode-size"_s,u"Invalid or unsafe barcode module size."_s,-1,u'^'+barcode.symbology});return;}
  QImage symbol(matrix->width*module,matrix->height*rowHeight,QImage::Format_ARGB32_Premultiplied);symbol.fill(options.background);
  QPainter painter(&symbol);painter.setPen(Qt::NoPen);painter.setBrush(options.foreground);
  for(int y=0;y<matrix->height;++y)for(int x=0;x<matrix->width;++x)if(matrix->at(x,y))painter.drawRect(x*module,y*rowHeight,module,rowHeight);
  painter.end();placeNewSymbol(image,symbol,state,barcode.orientation);
}

void drawBarcode(QImage& image, State& state, const Barcode& barcode, const FieldData& field, const RenderOptions& options, QList<Diagnostic>& diagnostics,RenderFonts& fonts) {
  if(state.reverse){
    QImage mask(image.size(),QImage::Format_ARGB32_Premultiplied);mask.fill(Qt::transparent);
    RenderOptions ink=options;ink.foreground=Qt::white;ink.background=Qt::transparent;
    state.reverse=false;drawBarcode(mask,state,barcode,field,ink,diagnostics,fonts);
    QPainter painter(&image);painter.setCompositionMode(QPainter::CompositionMode_Difference);painter.drawImage(0,0,mask);return;
  }
  const auto& data=field.data;
  const bool requiresBytes=barcode.symbology==u"BQ"||barcode.symbology==u"BX"||barcode.symbology==u"B7"
    ||barcode.symbology==u"BD"||barcode.symbology==u"BF"||barcode.symbology==u"BO"||barcode.symbology==u"B0";
  if(requiresBytes&&!field.bytes){
    const QString code=barcode.symbology==u"BQ"?u"qrcode-encoding"_s
      :barcode.symbology==u"BX"?u"datamatrix-encoding"_s
      :barcode.symbology==u"B7"?u"pdf417-encoding"_s
      :barcode.symbology==u"BD"?u"maxicode-encode"_s:u"barcode-encoding"_s;
    diagnostics.append({Severity::Error,code,
      u"The field cannot be represented as bytes in the selected character set; use ^CI28 for Unicode or ^FH for binary bytes."_s,
      -1,u'^'+barcode.symbology});
  }
  else if (barcode.symbology == u"B3") drawCode39(image,state,barcode,data,options,diagnostics);
  else if(barcode.symbology==u"BQ")drawQrCode(image,state,barcode,*field.bytes,options,diagnostics);
  else if(barcode.symbology==u"BX")drawDataMatrix(image,state,barcode,*field.bytes,options,diagnostics);
  else if(barcode.symbology==u"BE")drawEan13(image,state,barcode,data,options,diagnostics);
  else if(barcode.symbology==u"BC")drawCode128(image,state,barcode,data,options,diagnostics,fonts);
  else if(barcode.symbology==u"BK")drawCodabar(image,state,barcode,data,options,diagnostics);
  else if(barcode.symbology==u"B7")drawPdf417(image,state,barcode,*field.bytes,options,diagnostics);
  else if(barcode.symbology==u"BD")drawMaxiCode(image,state,barcode,*field.bytes,options,diagnostics);
  else if(QStringList{u"B8"_s,u"B9"_s,u"BU"_s,u"BA"_s,u"B2"_s,u"BI"_s}.contains(barcode.symbology))drawExtendedLinear(image,state,barcode,data,options,diagnostics,fonts);
  else if(QStringList{u"BF"_s,u"BR"_s,u"BO"_s,u"B0"_s}.contains(barcode.symbology))drawExtendedMatrix(image,state,barcode,field,options,diagnostics);
  else diagnostics.append({Severity::Warning,u"barcode-render-pending"_s,
    u"This barcode parser is available, but its native encoder is not implemented yet."_s,-1,u'^'+barcode.symbology});
  state.pendingBarcode.reset(); state.reverse=false; state.block.reset();
}

std::optional<FieldKind> drawingKind(const Command& command,const State& state) {
  return std::visit([&](const auto& value)->std::optional<FieldKind>{
    using T=std::decay_t<decltype(value)>;
    if constexpr(std::is_same_v<T,FieldData>)return state.pendingBarcode?FieldKind::Barcode:FieldKind::Text;
    else if constexpr(std::is_same_v<T,GraphicBox>)return FieldKind::Box;
    else if constexpr(std::is_same_v<T,GraphicCircle>)return FieldKind::Circle;
    else if constexpr(std::is_same_v<T,GraphicEllipse>)return FieldKind::Ellipse;
    else if constexpr(std::is_same_v<T,GraphicDiagonal>)return FieldKind::Diagonal;
    else if constexpr(std::is_same_v<T,GraphicField>)return FieldKind::Graphic;
    else if constexpr(std::is_same_v<T,UnknownCommand>)return FieldKind::Unknown;
    else return std::nullopt;
  },command.payload);
}

FieldSettings effectiveSettings(const State& state,const Command& command,QPoint home,int top,
                                Orientation printOrientation,bool mirror) {
  FieldSettings settings;
  settings.position=state.position;settings.baseline=state.baseline;
  settings.justification=state.justification;settings.font=state.font;settings.defaultFont=state.defaultFont;
  settings.fieldDirection=state.fieldDirection;settings.barcodeDefaults=state.barcodeDefaults;
  settings.barcode=state.pendingBarcode;settings.barcodeInterpretationFont=state.barcodeInterpretationFont;
  settings.block=state.block;settings.parameter=state.parameter;settings.characterSet=state.characterSet;
  settings.hexIndicator=state.hexIndicator;
  if(const auto* field=std::get_if<FieldData>(&command.payload)){
    settings.characterSet=field->characterSet;settings.hexIndicator=field->hexIndicator;
  }
  settings.labelHome=home;settings.labelShift=state.labelShift;settings.labelTop=top;
  settings.reverse=state.reverse;settings.printOrientation=printOrientation;settings.mirror=mirror;
  return settings;
}

QTransform outputTransform(QSize size,Orientation orientation,bool mirror) {
  QTransform transform;
  switch(orientation){
    case Orientation::Rotated90:transform=QTransform(0,1,-1,0,size.height(),0);break;
    case Orientation::Inverted:transform=QTransform(-1,0,0,-1,size.width(),size.height());break;
    case Orientation::BottomUp:transform=QTransform(0,-1,1,0,0,size.width());break;
    default:break;
  }
  if(mirror){
    const int width=(orientation==Orientation::Rotated90||orientation==Orientation::BottomUp)?size.height():size.width();
    transform=transform*QTransform(-1,0,0,1,width,0);
  }
  return transform;
}

std::expected<QImage, RenderError> interpretLabel(const Label& label, int index, const RenderOptions& options,
  QList<Diagnostic>& diagnostics,RenderFonts& fonts,bool rasterize,QList<FieldInfo>* fields,
  QList<FieldGeometry>* geometries,int initialCharacterSet) {
  const int width=options.width>0?options.width:(label.width()>0?label.width():812);
  const int height=options.height>0?options.height:(label.height()>0?label.height():1218);
  if(rasterize&&(width<=0||height<=0||width>100000||height>100000||qint64(width)*height>64*1024*1024)) return std::unexpected(RenderError{u"invalid-size"_s,u"Label dimensions are invalid or unsafe."_s,index});
  QImage image;
  if(rasterize){
    image=QImage(width,height,QImage::Format_ARGB32_Premultiplied);
    if(image.isNull())return std::unexpected(RenderError{u"raster-allocation"_s,u"The label raster could not be allocated."_s,index});
    image.fill(options.background);
  }
  State state;state.rasterize=rasterize;state.characterSet=initialCharacterSet;
  int labelTop=0;bool mirror=false;Orientation finalOrientation=Orientation::Normal;
  for(const auto& command:label.commands()){
    if(const auto* top=std::get_if<LabelTop>(&command.payload))labelTop=top->dots;
    if(const auto* value=std::get_if<PrintMirror>(&command.payload))mirror=value->enabled;
    if(const auto* value=std::get_if<PrintOrientation>(&command.payload))finalOrientation=value->orientation;
  }
  // An explicitly requested canvas centers a narrower/wider ^PW print area.
  // Apply this before ^PO/^PM, as the Labelary canvas does.
  const int printOffset=label.width()>0?int(std::floor((qint64(width)-label.width())/2.0)):0;
  const qint64 homeX=qint64(options.ignoreLabelHome?0:label.homeX())+printOffset;
  const qint64 homeY=qint64(options.ignoreLabelHome?0:label.homeY())+labelTop;

  qsizetype segmentStart=0,segmentEnd=-1;
  SourceSpan segmentSpan;bool segmentUnknown=false;
  const auto& commands=label.commands();
  QList<qsizetype> unknownOffsets;
  if(fields||geometries){
    for(const auto& diagnostic:diagnostics)
      if(diagnostic.code==u"unsupported-command")unknownOffsets.append(diagnostic.offset);
    std::sort(unknownOffsets.begin(),unknownOffsets.end());
  }
  const QTransform transform=outputTransform(QSize(width,height),finalOrientation,mirror);
  const QRect outputCanvas=transform.mapRect(QRect(0,0,width,height));
  for(qsizetype commandIndex=0;commandIndex<commands.size();++commandIndex) {
    if(options.stopToken.stop_requested())return std::unexpected(RenderError{u"operation-cancelled"_s,u"The operation was cancelled."_s,index});
    const auto& command=commands[commandIndex];
    state.reverse=state.labelReverse||state.fieldReverse;
    const auto kind=drawingKind(command,state);
    std::optional<FieldInfo> field;
    State::Observation observation;
    state.observation=geometries&&kind?&observation:nullptr;
    if((fields||geometries)&&kind){
      if(segmentEnd<commandIndex){
        segmentStart=commandIndex;
        while(segmentStart>0&&!std::holds_alternative<FieldSeparator>(commands[segmentStart-1].payload)
          &&!std::holds_alternative<FormatStart>(commands[segmentStart-1].payload))--segmentStart;
        segmentEnd=commandIndex;segmentUnknown=false;
        while(segmentEnd+1<commands.size()&&!std::holds_alternative<FieldSeparator>(commands[segmentEnd].payload)
          &&!std::holds_alternative<FormatEnd>(commands[segmentEnd+1].payload))++segmentEnd;
        // Omitted unknown commands still belong to this source segment. Start
        // at the preceding field/format boundary, not the first saved command.
        const auto boundary=segmentStart>0?commands[segmentStart-1].sourceSpan:label.sourceSpan();
        const qsizetype sourceStart=segmentStart>0?boundary.start+boundary.length:boundary.start;
        const auto end=commands[segmentEnd].sourceSpan;
        segmentSpan={sourceStart,end.start+end.length-sourceStart};
        for(qsizetype i=segmentStart;i<=segmentEnd;++i)
          segmentUnknown|=std::holds_alternative<UnknownCommand>(commands[i].payload);
        if(!segmentUnknown){
          const auto unknown=std::lower_bound(unknownOffsets.cbegin(),unknownOffsets.cend(),segmentSpan.start);
          segmentUnknown=unknown!=unknownOffsets.cend()&&*unknown<segmentSpan.start+segmentSpan.length;
        }
      }
      field=FieldInfo{command.offset,index,commandIndex,segmentSpan,command.sourceSpan,*kind,
        effectiveSettings(state,command,QPoint(label.homeX(),label.homeY()),labelTop,finalOrientation,mirror),segmentUnknown};
      if(fields)fields->append(*field);
    }
    const qsizetype firstDiagnostic=diagnostics.size();
    std::visit([&](const auto& value){
      using T=std::decay_t<decltype(value)>;
      if constexpr(std::is_same_v<T,FieldOrigin>){state.position=printerPosition(qint64(value.x)+state.labelShift+homeX,qint64(value.y)+homeY);state.baseline=false;state.justification=value.useDefaultJustification?state.defaultJustification:value.justification;}
      else if constexpr(std::is_same_v<T,FieldTypeset>){state.position=printerPosition(value.usePreviousX?state.nextTypeset.x():qint64(value.x)+state.labelShift+homeX,value.usePreviousY?state.nextTypeset.y():qint64(value.y)+homeY);state.baseline=true;state.justification=value.useDefaultJustification?state.defaultJustification:value.justification;}
      else if constexpr(std::is_same_v<T,ScalableFont>) {state.font=value;state.barcodeInterpretationFont=value;}
      else if constexpr(std::is_same_v<T,FieldSeparator>) {state.font=state.defaultFont;state.barcodeInterpretationFont.reset();state.fieldReverse=false;state.parameter={};state.pendingBarcode.reset();state.block.reset();state.hexIndicator={};}
      else if constexpr(std::is_same_v<T,ChangeFont>){state.defaultFont={value.font,Orientation::Normal,value.height,value.width};state.font=state.defaultFont;}
      else if constexpr(std::is_same_v<T,FieldDirection>){state.fieldDirection=value.orientation;state.defaultJustification=value.justification;}
      else if constexpr(std::is_same_v<T,FieldReverse>) state.fieldReverse=true;
      else if constexpr(std::is_same_v<T,LabelReverse>) state.labelReverse=value.enabled;
      else if constexpr(std::is_same_v<T,FieldParameter>) state.parameter=value;
      else if constexpr(std::is_same_v<T,FieldBlock>) state.block=value;
      else if constexpr(std::is_same_v<T,FieldEncoding>) { /* Accepted compatibility command; no raster state change. */ }
      else if constexpr(std::is_same_v<T,CharacterSet>) state.characterSet=value.id;
      else if constexpr(std::is_same_v<T,FieldHex>) state.hexIndicator=value.indicator;
      else if constexpr(std::is_same_v<T,BarcodeDefault>) state.barcodeDefaults=value;
      else if constexpr(std::is_same_v<T,LabelShift>) state.labelShift=value.dots;
      else if constexpr(std::is_same_v<T,PrintMode>) { /* Print mode does not change local raster geometry. */ }
      else if constexpr(std::is_same_v<T,PrintOrientation>) state.printOrientation=value.orientation;
      else if constexpr(std::is_same_v<T,Barcode>){
        state.pendingBarcode=value;state.barcodeOffset=command.offset;state.barcodeSource=command.source;
        // QR magnification also changes the following barcode module width,
        // even if this ^BQ has no field data. ^BY subsequently overrides it.
        if(value.symbology==u"BQ")state.barcodeDefaults.moduleWidth=qrMagnification(value);
      }
      else if constexpr(std::is_same_v<T,FieldData>){
        if(state.pendingBarcode){
          if(!rasterize){state.pendingBarcode.reset();state.reverse=false;state.block.reset();return;}
          const auto firstDiagnostic=diagnostics.size();
          drawBarcode(image,state,*state.pendingBarcode,value,options,diagnostics,fonts);
          for(qsizetype i=firstDiagnostic;i<diagnostics.size();++i){
            if(diagnostics[i].offset<0)diagnostics[i].offset=state.barcodeOffset;
            diagnostics[i].command=state.barcodeSource;
          }
        }else{
          const auto firstDiagnostic=diagnostics.size();
          drawText(image,state,value.data,options,fonts,diagnostics,command.offset);
          for(qsizetype i=firstDiagnostic;i<diagnostics.size();++i)diagnostics[i].command=command.source;
        }
      }
      else if constexpr(std::is_same_v<T,GraphicBox>){
        if(value.width<0||value.height<0||value.width>32000||value.height>32000||value.thickness>32000){diagnostics.append({Severity::Error,u"graphic-dimensions"_s,u"Graphic dimensions must be between 0 and 32000 dots."_s,command.offset,command.source});state.reverse=false;return;}
        const int boxWidth=value.width>0?value.width:value.thickness;
        const int boxHeight=value.height>0?value.height:value.thickness;
        const QPoint origin=state.position-QPoint(state.justification==Justification::Right?boxWidth:0,state.baseline?boxHeight:0);
        const auto fillRectangle=QRect(origin,QSize(boxWidth,boxHeight));
        observeLogical(state,fillRectangle);
        if(state.reverse||commandColor(value.color,options).alpha()>0)observePaint(state,fillRectangle);
        if(!rasterize){state.reverse=false;return;}
        const bool filled=qint64(value.thickness)*2>=std::min(boxWidth,boxHeight);
        QPainterPath path;path.setFillRule(Qt::OddEvenFill);path.addRect(QRectF(fillRectangle));
        if(!filled){const int inset=std::min({value.thickness,boxWidth/2,boxHeight/2});path.addRect(QRectF(fillRectangle.adjusted(inset,inset,-inset,-inset)));}
        QPainter p(&image);p.setRenderHint(QPainter::Antialiasing,false);p.setPen(Qt::NoPen);p.setBrush(state.reverse?Qt::white:commandColor(value.color,options));
        if(state.reverse)p.setCompositionMode(QPainter::CompositionMode_Difference);p.drawPath(path);
        state.reverse=false;
      }
      else if constexpr(std::is_same_v<T,GraphicCircle>||std::is_same_v<T,GraphicEllipse>){
        const int width=[](const auto& graphic){if constexpr(std::is_same_v<T,GraphicCircle>)return graphic.diameter;else return graphic.width;}(value);
        const int height=[](const auto& graphic){if constexpr(std::is_same_v<T,GraphicCircle>)return graphic.diameter;else return graphic.height;}(value);
        if(width<0||height<0||width>32000||height>32000||value.thickness>32000){diagnostics.append({Severity::Error,u"graphic-dimensions"_s,u"Graphic dimensions must be between 0 and 32000 dots."_s,command.offset,command.source});state.reverse=false;return;}
        const QPoint origin=GraphicGeometry::anchoredOrigin(state.position,width,height,state.justification==Justification::Right,state.baseline);
        observeLogical(state,QRect(origin,QSize(width,height)));
        if(state.observation&&width>0&&height>0&&(state.reverse||commandColor(value.color,options).alpha()>0)){
          const auto outer=GraphicGeometry::ellipse(origin,width,height,false);
          QRect paint;
          for(int y=origin.y();y<qint64(origin.y())+height;++y){
            const auto [left,right]=GraphicGeometry::span(outer,y);
            if(right>left)paint|=QRect(left,y,right-left,1);
          }
          observePaint(state,paint);
        }
        if(!rasterize){state.reverse=false;return;}
        QPainter p(&image);p.setRenderHint(QPainter::Antialiasing,false);p.setPen(Qt::NoPen);p.setBrush(state.reverse?Qt::white:commandColor(value.color,options));
        if(state.reverse)p.setCompositionMode(QPainter::CompositionMode_Difference);
        GraphicGeometry::drawEllipse(p,origin,width,height,value.thickness,image.size());state.reverse=false;
      }
      else if constexpr(std::is_same_v<T,GraphicDiagonal>){
        if(value.width<0||value.height<0||value.width>32000||value.height>32000||value.thickness>32000){diagnostics.append({Severity::Error,u"graphic-dimensions"_s,u"Graphic dimensions must be between 0 and 32000 dots."_s,command.offset,command.source});state.reverse=false;return;}
        const QPoint origin=GraphicGeometry::anchoredOrigin(state.position,value.width,value.height,state.justification==Justification::Right,state.baseline);
        observeLogical(state,QRect(origin,QSize(value.width,value.height)));
        if(value.width>0&&value.height>0&&value.thickness>0&&(state.reverse||commandColor(value.color,options).alpha()>0)){
          const qint64 step=qint64(value.width)*1024/value.height;
          const qint64 last=step*(value.height-1);
          const int firstX=value.orientation==u'L'?0:int((qint64(value.width)*1024-last+1023)/1024);
          const int lastX=value.orientation==u'L'?int((last+1023)/1024):value.width;
          observePaint(state,QRect(origin+QPoint(firstX,0),QSize(lastX-firstX+value.thickness,value.height)));
        }
        if(!rasterize){state.reverse=false;return;}
        QPainter p(&image);p.setPen(Qt::NoPen);p.setBrush(state.reverse?Qt::white:commandColor(value.color,options));
        if(state.reverse)p.setCompositionMode(QPainter::CompositionMode_Difference);
        GraphicGeometry::drawDiagonal(p,origin,value.width,value.height,value.thickness,value.orientation==u'L',image.size());
        state.reverse=false;
      }
      else if constexpr(std::is_same_v<T,GraphicField>) {
        if(!rasterize)return;
        const auto decoded=decodeGraphic(value,{options.maxGraphicBytes,options.stopToken});
        if(!decoded){
          diagnostics.append({Severity::Error,decoded.error().code,decoded.error().message,command.offset,command.source});return;
        }
        QPoint position=state.position;
        if(state.baseline)position=printerPosition(position.x(),qint64(position.y())-decoded->size.height());
        if(state.justification==Justification::Right)position=printerPosition(qint64(position.x())-decoded->size.width(),position.y());
        if(state.reverse){
          QImage mask(image.size(),QImage::Format_ARGB32_Premultiplied);mask.fill(Qt::transparent);
          auto ink=options;ink.background=Qt::transparent;ink.foreground=Qt::white;
          drawGraphicField(mask,position,*decoded,ink,state);
          QPainter painter(&image);painter.setCompositionMode(QPainter::CompositionMode_Difference);painter.drawImage(0,0,mask);
        }else drawGraphicField(image,position,*decoded,options,state);
      }
    },command.payload);
    if(const auto fontError=fonts.takeFont0Error();!fontError.isEmpty())
      diagnostics.append({Severity::Error,u"font0-outline"_s,fontError,command.offset,command.source});
    if(options.stopToken.stop_requested())return std::unexpected(RenderError{u"operation-cancelled"_s,u"The operation was cancelled."_s,index});
    bool error=false,unsupported=kind==FieldKind::Unknown;
    for(qsizetype i=firstDiagnostic;i<diagnostics.size();++i){
      auto& diagnostic=diagnostics[i];diagnostic.labelIndex=index;
      if(kind)diagnostic.fieldId=command.offset;
      if(diagnostic.offset<0)diagnostic.offset=command.offset;
      if(!diagnostic.sourceSpan){
        const auto source=std::lower_bound(commands.cbegin(),commands.cend(),diagnostic.offset,
          [](const Command& candidate,qsizetype offset){return candidate.offset<offset;});
        if(source!=commands.cend()&&source->offset==diagnostic.offset){
          diagnostic.sourceSpan=source->sourceSpan;diagnostic.command=source->source;
        }
      }
      if(diagnostic.command.isEmpty())diagnostic.command=command.source;
      error|=diagnostic.severity==Severity::Error;
      unsupported|=diagnostic.code==u"barcode-render-pending";
    }
    if(diagnostics.size()>options.maxDiagnostics)
      return std::unexpected(RenderError{u"render-diagnostic-limit"_s,u"The diagnostic budget was exceeded."_s,index});
    if(geometries&&field){
      FieldGeometry geometry;geometry.field=std::move(*field);
      geometry.logicalBounds=transform.mapRect(observation.logicalBounds);
      geometry.paintBounds=transform.mapRect(observation.paintBounds);
      geometry.clippedBounds=geometry.paintBounds.intersected(outputCanvas);
      geometry.anchor=transform.map(geometry.field.settings.position);
      geometry.baseline=geometry.field.settings.baseline;geometry.labelTransform=transform;
      geometry.status=error?FieldStatus::Error:unsupported?FieldStatus::Unsupported:
        geometry.paintBounds.isEmpty()?FieldStatus::Empty:geometry.clippedBounds.isEmpty()?FieldStatus::Clipped:FieldStatus::Drawn;
      geometries->append(std::move(geometry));
    }
  }
  if(!rasterize)return QImage{};
  image=rotateImage(image,state.printOrientation);
  if(mirror)image.flip(Qt::Horizontal);
  return image;
}

} // namespace

namespace {
int nextCharacterSet(const Label& label,int previous) {
  for(const auto& command:label.commands())
    if(const auto* value=std::get_if<CharacterSet>(&command.payload))previous=value->id;
  return previous;
}

int initialCharacterSet(const Document& document,int labelIndex) {
  int result=0;
  for(int i=0;i<labelIndex;++i)result=nextCharacterSet(document.labels()[i],result);
  return result;
}

std::expected<void,RenderError> validateOperation(const Document& document,const RenderOptions& options,
  std::optional<int> selected,bool rasterize) {
  if(options.stopToken.stop_requested())
    return std::unexpected(RenderError{u"operation-cancelled"_s,u"The operation was cancelled."_s,selected.value_or(-1)});
  if(options.dpi!=203&&options.dpi!=300&&options.dpi!=600)
    return std::unexpected(RenderError{u"invalid-dpi"_s,u"DPI must be 203, 300, or 600."_s,-1});
  if(selected&&(*selected<0||*selected>=document.labels().size()))
    return std::unexpected(RenderError{u"invalid-label-index"_s,u"The label index is outside the document."_s,*selected});
  if(options.width<0||options.height<0||options.maxTotalPixels<0||options.maxGraphicBytes<0||options.maxDiagnostics<0)
    return std::unexpected(RenderError{u"invalid-render-options"_s,u"Render limits and dimensions must not be negative."_s,-1});
  if(document.diagnostics().size()>options.maxDiagnostics)
    return std::unexpected(RenderError{u"render-diagnostic-limit"_s,u"The diagnostic budget was exceeded."_s,-1});
  qint64 pixels=0,graphicBytes=0;
  const qsizetype first=selected.value_or(0),last=selected?*selected+1:document.labels().size();
  for(qsizetype i=first;i<last;++i){
    const auto& label=document.labels()[i];
    if(rasterize){
      const qint64 width=options.width>0?options.width:label.width()>0?label.width():812;
      const qint64 height=options.height>0?options.height:label.height()>0?label.height():1218;
      if(width>100000||height>100000||width*height>64*1024*1024)
        return std::unexpected(RenderError{u"invalid-size"_s,u"Label dimensions are invalid or unsafe."_s,int(i)});
      if(width*height>options.maxTotalPixels-pixels)
        return std::unexpected(RenderError{u"render-pixel-limit"_s,u"The total output pixel budget was exceeded."_s,int(i)});
      pixels+=width*height;
      for(const auto& command:label.commands()){
        if(options.stopToken.stop_requested())
          return std::unexpected(RenderError{u"operation-cancelled"_s,u"The operation was cancelled."_s,int(i)});
        if(const auto* graphic=std::get_if<GraphicField>(&command.payload)){
          const qint64 bytes=graphic->bytesPerRow>0&&graphic->totalBytes>0
            ?((qint64(graphic->totalBytes)+graphic->bytesPerRow-1)/graphic->bytesPerRow)*graphic->bytesPerRow:0;
          if(bytes>options.maxGraphicBytes-graphicBytes)
            return std::unexpected(RenderError{u"render-graphic-limit"_s,u"The total decoded graphic budget was exceeded."_s,int(i)});
          graphicBytes+=bytes;
        }
      }
    }
  }
  return {};
}
}

std::expected<RenderResult, RenderError> render(const Document& document, const RenderOptions& options) {
  if(auto valid=validateOperation(document,options,std::nullopt,true);!valid)return std::unexpected(valid.error());
  RenderResult result;result.diagnostics=document.diagnostics();result.labels.reserve(document.labels().size());
  RenderFonts fonts;int characterSet=0;
  for(int i=0;i<document.labels().size();++i){
    auto image=interpretLabel(document.labels()[i],i,options,result.diagnostics,fonts,true,nullptr,
      options.collectFieldGeometry?&result.fields:nullptr,characterSet);
    if(!image)return std::unexpected(image.error());
    result.labels.append(std::move(*image));
    characterSet=nextCharacterSet(document.labels()[i],characterSet);
  }
  return result;
}

std::expected<LabelRenderResult,RenderError> renderLabel(const Document& document,int labelIndex,const RenderOptions& options) {
  if(auto valid=validateOperation(document,options,labelIndex,true);!valid)return std::unexpected(valid.error());
  LabelRenderResult result;result.labelIndex=labelIndex;result.diagnostics=document.diagnostics();
  RenderFonts fonts;
  auto image=interpretLabel(document.labels()[labelIndex],labelIndex,options,result.diagnostics,fonts,true,nullptr,
    options.collectFieldGeometry?&result.fields:nullptr,initialCharacterSet(document,labelIndex));
  if(!image)return std::unexpected(image.error());
  result.image=std::move(*image);
  return result;
}

std::expected<AnalysisResult,RenderError> analyze(const Document& document,const RenderOptions& options) {
  if(auto valid=validateOperation(document,options,std::nullopt,false);!valid)return std::unexpected(valid.error());
  AnalysisResult result;result.diagnostics=document.diagnostics();RenderFonts fonts;int characterSet=0;
  for(int i=0;i<document.labels().size();++i){
    auto analyzed=interpretLabel(document.labels()[i],i,options,result.diagnostics,fonts,false,&result.fields,nullptr,characterSet);
    if(!analyzed)return std::unexpected(analyzed.error());
    characterSet=nextCharacterSet(document.labels()[i],characterSet);
  }
  return result;
}

std::expected<AnalysisResult,RenderError> analyzeLabel(const Document& document,int labelIndex,const RenderOptions& options) {
  if(auto valid=validateOperation(document,options,labelIndex,false);!valid)return std::unexpected(valid.error());
  AnalysisResult result;result.diagnostics=document.diagnostics();RenderFonts fonts;
  auto analyzed=interpretLabel(document.labels()[labelIndex],labelIndex,options,result.diagnostics,fonts,false,&result.fields,nullptr,initialCharacterSet(document,labelIndex));
  if(!analyzed)return std::unexpected(analyzed.error());
  return result;
}

std::expected<RenderResult, Error> render(QStringView zpl, const ParseOptions& parseOptions, const RenderOptions& renderOptions) {
  auto document=parse(zpl,parseOptions);
  if(!document)return std::unexpected(Error{Error::Stage::Parse,document.error().code,document.error().message,document.error().offset,-1});
  auto result=render(*document,renderOptions);
  if(!result)return std::unexpected(Error{Error::Stage::Render,result.error().code,result.error().message,-1,result.error().labelIndex});
  return *result;
}

} // namespace QtZpl
