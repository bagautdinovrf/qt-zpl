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

#include <QtCore/QByteArray>
#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QtMath>
#include <QtCore/QtEndian>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QFontDatabase>
#include <QtGui/QTransform>
#include <QtGui/QRawFont>
#include <algorithm>
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
};

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

struct BlockLine {
  QString text;
  int indent = 0;
  bool paragraphEnd = false;
};

// A QRawFont owns a font engine for the current rendering thread. Keep one
// lazily-created face per render call, including all labels in that call,
// without sharing a mutable font engine between concurrent document renders.
class RenderFonts {
public:
  const QRawFont& font0() {
    if(!font0_)font0_.emplace(font0Bytes(),1000,QFont::PreferNoHinting);
    return *font0_;
  }
  const QPainterPath& font0Glyph(quint32 glyph) {
    const auto found=font0Glyphs_.constFind(glyph);
    if(found!=font0Glyphs_.cend())return found.value();
    return font0Glyphs_.insert(glyph,font0().pathForGlyph(glyph)).value();
  }
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
  std::optional<QRawFont> font0_;
  std::optional<PrinterFontRaster> fontARaster_;
  // Keys come only from the embedded face, bounding this cache by its glyph
  // count. Store design-space paths so all requested sizes share exact data.
  QHash<quint32,QPainterPath> font0Glyphs_;
};

// Use the font's design grid rather than QFont::setStretch. On Windows the
// latter quantizes widths through GDI's integer average-character width, while
// other Qt backends use a different scale. Font 0 has a 1000-unit em and a
// 750-unit cap height; independent ZPL height/width scale that same design grid.
class Font0Metrics {
public:
  Font0Metrics(int height,int width,RenderFonts& fontContext,FieldParameter parameter={})
    : font(fontContext.font0()),fonts(fontContext),
      scaleX(static_cast<qreal>(width>0?width:height)/1000.0),
      scaleY(static_cast<qreal>(height)/1000.0),fontHeight(height),parameter(parameter) {}

  [[nodiscard]] int ascent() const {return qFloor(font.capHeight()*scaleY);}
  [[nodiscard]] int height() const {return std::max(1,fontHeight);}
  [[nodiscard]] qreal designAdvance(const QString& text) const {
    const auto glyphs=font.glyphIndexesForString(text);
    const auto advances=font.advancesForGlyphIndexes(glyphs,QRawFont::UseDesignMetrics);
    qreal width=0;for(const auto& advance:advances)width+=advance.x()*scaleX;
    return width+std::max<qsizetype>(0,glyphs.size()-1)*parameter.spacing;
  }
  [[nodiscard]] int horizontalAdvance(const QString& text) const {return qRound(designAdvance(text));}
  [[nodiscard]] QPainterPath outline(const QString& text) const {
    const auto glyphs=font.glyphIndexesForString(text);
    const auto advances=font.advancesForGlyphIndexes(glyphs,QRawFont::UseDesignMetrics);
    QPainterPath result;qreal x=parameter.direction==u'R'?-parameter.spacing:0,y=0;
    for(qsizetype i=0;i<glyphs.size();++i){
      QTransform transform;transform.translate(x,y);transform.scale(scaleX,scaleY);
      result.addPath(transform.map(fonts.font0Glyph(glyphs[i])));
      if(parameter.direction==u'V')y+=fontHeight;
      else x+=(parameter.direction==u'R'?-1:1)*(advances[i].x()*scaleX+parameter.spacing);
    }
    return result;
  }
  void draw(QPainter& painter,int x,int baseline,const QString& text,const QColor& color) const {
    const auto path=outline(text).translated(x,baseline);
    painter.fillPath(path,color);
  }
private:
  const QRawFont& font;
  RenderFonts& fonts;
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

// Barcode/graphic anchors lie on cell edges; glyph baseline coordinates above
// instead refer to individual raster dots.
QPoint rotatedEdge(QPoint anchor,QSize source,Orientation orientation) {
  switch(orientation){
    case Orientation::Rotated90:return {source.height()-anchor.y(),anchor.x()};
    case Orientation::Inverted:return {source.width()-anchor.x(),source.height()-anchor.y()};
    case Orientation::BottomUp:return {anchor.y(),source.width()-anchor.x()};
    default:return anchor;
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
  const QPoint anchor=state.baseline?rotatedAnchor(logicalAnchor,layoutSize,orientation):QPoint{};
  QPoint rasterOffset;
  if(orientation==Orientation::Rotated90)rasterOffset.rx()=layoutHeight-actualHeight;
  else if(orientation==Orientation::Inverted)rasterOffset.ry()=layoutHeight-actualHeight;
  field=rotateImage(field,orientation);
  QPainter painter(&image);
  painter.setCompositionMode(state.reverse?QPainter::CompositionMode_Difference:QPainter::CompositionMode_SourceOver);
  painter.drawImage(state.position-anchor+rasterOffset,field);
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

  QPainterPath outline=metrics.outline(text);
  if(!state.baseline)outline.translate(0,metrics.ascent());
  const QRect bounds=outline.boundingRect().toAlignedRect();
  if(bounds.isEmpty()){state.reverse=false;state.block.reset();return;}
  QImage field(bounds.size(),QImage::Format_ARGB32_Premultiplied);
  field.fill(Qt::transparent);
  QPainter raster(&field);raster.setRenderHint(QPainter::Antialiasing,false);
  raster.fillPath(outline.translated(-bounds.left(),-bounds.top()),color);raster.end();

  QPoint position=state.position;
  if(state.baseline){
    const QPoint baseline(-bounds.left(),-bounds.top());
    position-=rotatedAnchor(baseline,field.size(),orientation);
  }else{
    // ^FO rotates the nominal character cell, retaining side bearings and
    // descenders. It is not the top-left of the cropped ink. Glyphs may extend
    // beyond that cell, so the raster bounds must remain independent of it.
    switch(orientation){
      case Orientation::Normal:position+=bounds.topLeft();break;
      case Orientation::Rotated90:position+=QPoint(height-bounds.bottom()-1,bounds.left());break;
      case Orientation::Inverted:position+=QPoint(qFloor(metrics.designAdvance(text))-bounds.right()-1,height-bounds.bottom()-1);break;
      case Orientation::BottomUp:position+=QPoint(bounds.top(),qFloor(metrics.designAdvance(text))-bounds.right()-1);break;
    }
  }
  field=rotateImage(field,orientation);
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
  QPainter target(&image);if(state.reverse)target.setCompositionMode(QPainter::CompositionMode_Difference);
  target.drawImage(position,rotateImage(field,orientation));
}

void drawTextField(QImage& image, State& state, const QString& text, const RenderOptions& options,RenderFonts& fonts,
                   QList<Diagnostic>& diagnostics,qsizetype offset) {
  if (text.isEmpty()) { state.reverse=false; state.block.reset(); return; }
  const QSize defaults=builtInFontDefaults(state.font.font);
  const int height = state.font.height>0?state.font.height:defaults.height();
  const int width = state.font.width>0?state.font.width:
    state.font.font.toUpper()==u'A'?std::max(1,qRound(height/9.0))*5:defaults.width();
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
  painter.setCompositionMode(state.reverse ? QPainter::CompositionMode_Difference : QPainter::CompositionMode_SourceOver);
  painter.drawImage(pos, field);
  state.reverse=false; state.block.reset();
}

void drawText(QImage& image,State& state,const QString& text,const RenderOptions& options,RenderFonts& fonts,
              QList<Diagnostic>& diagnostics,qsizetype offset) {
  const auto defaults=builtInFontDefaults(state.font.font);
  const int height=state.font.height>0?state.font.height:defaults.height();
  const int width=state.font.width>0?state.font.width:
    state.font.font.toUpper()==u'A'?std::max(1,qRound(height/9.0))*5:defaults.width();
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
  int advance=0,ascent=height;
  if(state.font.font==u'0'){
    Font0Metrics metrics(height,width,fonts,state.parameter);advance=metrics.horizontalAdvance(text);ascent=metrics.ascent();
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
    if(state.baseline)placed.position-=orientedVector(QPoint(advance,0),orientation);
    else placed.position.rx()-=(orientation==Orientation::Rotated90||orientation==Orientation::BottomUp?height:advance);
  }
  const int cursorAdvance=state.font.font.toUpper()==u'A'
    ?qFloor(std::max(1,qRound(width/5.0))*9.0*1365.0/2048*text.size()+state.parameter.spacing*std::max(0,int(text.size())-1))
    :advance;
  const QPoint end=state.parameter.direction==u'V'?QPoint(0,height*int(text.size()))
    :QPoint(state.parameter.direction==u'R'?-cursorAdvance:cursorAdvance,0);
  state.nextTypeset=placed.position+orientedVector(end+QPoint(0,state.baseline?0:ascent),orientation);
  drawTextField(image,placed,text,options,fonts,diagnostics,offset);
  state.reverse=false;state.block.reset();
}

quint16 z64Crc(QByteArrayView encoded) {
  quint16 crc=0;
  for(const unsigned char byte:encoded){
    crc^=static_cast<quint16>(byte)<<8;
    for(int bit=0;bit<8;++bit)crc=crc&0x8000U?static_cast<quint16>((crc<<1)^0x1021U):static_cast<quint16>(crc<<1);
  }
  return crc;
}

std::optional<QByteArray> decodeZ64(const GraphicField& gf,qsizetype offset,QList<Diagnostic>& diagnostics) {
  constexpr int maximumGraphicBytes=64*1024*1024;
  const auto fail=[&](QString code,QString message)->std::optional<QByteArray>{
    diagnostics.append({Severity::Error,std::move(code),std::move(message),offset,u"^GF"_s});
    return std::nullopt;
  };
  if(gf.totalBytes<=0||gf.totalBytes>maximumGraphicBytes)
    return fail(u"graphic-field-z64-size"_s,u"The Z64 graphic declares an invalid or unsafe decompressed size."_s);

  const auto data=QByteArrayView{gf.data}.trimmed();
  constexpr QByteArrayView prefix{":Z64:"};
  const auto checksumSeparator=data.lastIndexOf(':');
  if(!data.startsWith(prefix)||checksumSeparator<=prefix.size()||data.size()-checksumSeparator-1!=4)
    return fail(u"graphic-field-z64-format"_s,u"The Z64 graphic must use :Z64:<Base64>:<CRC16>."_s);

  const auto encoded=data.sliced(prefix.size(),checksumSeparator-prefix.size());
  bool checksumOk=false;
  const auto expectedCrc=QString::fromLatin1(data.sliced(checksumSeparator+1)).toUShort(&checksumOk,16);
  if(!checksumOk)
    return fail(u"graphic-field-z64-format"_s,u"The Z64 graphic CRC must contain four hexadecimal digits."_s);
  if(z64Crc(encoded)!=expectedCrc)
    return fail(u"graphic-field-z64-crc"_s,u"The Z64 graphic CRC does not match its Base64-encoded data."_s);

  const auto decoded=QByteArray::fromBase64Encoding(encoded.toByteArray(),
    QByteArray::Base64Encoding|QByteArray::AbortOnBase64DecodingErrors);
  if(!decoded)
    return fail(u"graphic-field-z64-base64"_s,u"The Z64 graphic contains invalid Base64 data."_s);

  QByteArray compressedWithSize(4,Qt::Uninitialized);
  qToBigEndian<quint32>(static_cast<quint32>(gf.totalBytes),compressedWithSize.data());
  compressedWithSize.append(decoded.decoded);
  auto uncompressed=qUncompress(compressedWithSize);
  if(uncompressed.size()!=gf.totalBytes)
    return fail(u"graphic-field-z64-zlib"_s,u"The Z64 graphic could not be decompressed to its declared size."_s);
  return uncompressed;
}

int asciiCompressionRepeatCount(char code) {
  if(code>='G'&&code<='Y')return code-'G'+1;
  if(code>='g'&&code<='z')return (code-'g'+1)*20;
  return 0;
}

std::optional<QByteArray> decodeAsciiGraphic(const GraphicField& gf,qsizetype offset,
                                             QList<Diagnostic>& diagnostics) {
  const auto fail=[&](QString message)->std::optional<QByteArray>{
    diagnostics.append({Severity::Error,u"graphic-field-ascii-compression"_s,
      std::move(message),offset,u"^GFA"_s});
    return std::nullopt;
  };
  constexpr qsizetype maximumGraphicBytes=64*1024*1024;
  if(gf.totalBytes<=0||gf.bytesPerRow<=0)
    return fail(u"The graphic byte count and bytes-per-row must be positive."_s);
  if(gf.totalBytes%gf.bytesPerRow!=0)
    return fail(u"The graphic byte count is not an exact number of rows."_s);
  if(gf.totalBytes>maximumGraphicBytes)
    return fail(u"The expanded graphic exceeds the safe 64 MiB limit."_s);

  const qsizetype rowNibbles=static_cast<qsizetype>(gf.bytesPerRow)*2;
  const qsizetype expectedBytes=gf.totalBytes;
  QByteArray decoded;decoded.reserve(gf.totalBytes);
  QByteArray row;row.reserve(rowNibbles);
  QByteArray previousRow;
  const auto finishRow=[&]()->bool{
    if(row.size()!=rowNibbles)return false;
    const auto bytes=QByteArray::fromHex(row);
    if(bytes.size()!=gf.bytesPerRow||decoded.size()+bytes.size()>expectedBytes)return false;
    decoded.append(bytes);previousRow=bytes;row.clear();return true;
  };
  const auto appendNibbles=[&](char nibble,qsizetype count)->bool{
    if(count<=0||count>rowNibbles-row.size())return false;
    row.append(count,nibble);
    return row.size()!=rowNibbles||finishRow();
  };

  for(qsizetype i=0;i<gf.data.size();){
    const char code=gf.data[i];
    if(code==' '||code=='\t'||code=='\r'||code=='\n'){++i;continue;}
    if(code==',') { // Fill the remainder of this row with zero nibbles.
      const auto remaining=rowNibbles-row.size();++i;
      if(remaining<=0||!appendNibbles('0',remaining))
        return fail(u"A zero-fill marker overflowed its row."_s);
      continue;
    }
    if(code=='!') {
      const auto remaining=rowNibbles-row.size();++i;
      if(remaining<=0||!appendNibbles('F',remaining))
        return fail(u"A one-fill marker overflowed its row."_s);
      continue;
    }
    if(code==':') {
      ++i;
      if(!row.isEmpty()||previousRow.size()!=gf.bytesPerRow
         ||decoded.size()+previousRow.size()>expectedBytes)
        return fail(u"A repeat-row marker has no complete preceding row or overflows the graphic."_s);
      decoded.append(previousRow);
      continue;
    }
    if(const int firstCount=asciiCompressionRepeatCount(code);firstCount>0){
      qsizetype count=0;
      while(i<gf.data.size()){
        const int part=asciiCompressionRepeatCount(gf.data[i]);
        if(part==0)break;
        count+=part;++i;
      }
      if(i>=gf.data.size())return fail(u"A repeat count has no following hexadecimal nibble."_s);
      const char nibble=gf.data[i++];
      const bool hexadecimal=(nibble>='0'&&nibble<='9')||(nibble>='A'&&nibble<='F')
        ||(nibble>='a'&&nibble<='f');
      if(!hexadecimal)return fail(u"A repeat count is not followed by a hexadecimal nibble."_s);
      if(!appendNibbles(nibble,count))return fail(u"Repeated hexadecimal data overflowed its row."_s);
      continue;
    }
    const bool hexadecimal=(code>='0'&&code<='9')||(code>='A'&&code<='F')
      ||(code>='a'&&code<='f');
    if(!hexadecimal)return fail(u"The compressed graphic contains an invalid character."_s);
    ++i;
    if(!appendNibbles(code,1))return fail(u"Hexadecimal data overflowed its row."_s);
  }
  if(!row.isEmpty()){
    if(row.size()%2!=0)return fail(u"The compressed graphic ends with an unmatched hexadecimal nibble."_s);
    return fail(u"The final graphic row contains fewer bytes than declared."_s);
  }
  // Zebra and Labelary treat omitted complete trailing rows as blank. Materialize
  // those rows so the validated buffer still has exactly the declared size.
  if(decoded.size()<expectedBytes)decoded.append(expectedBytes-decoded.size(),char{0});
  if(decoded.size()!=expectedBytes)
    return fail(u"The expanded graphic size exceeds the declared total byte count."_s);
  return decoded;
}

void drawGraphicField(QImage& image,const QPoint& pos,const GraphicField& gf,const RenderOptions& options,
                      qsizetype offset,QList<Diagnostic>& diagnostics) {
  if (gf.bytesPerRow <= 0) return;
  const bool z64=gf.compression==u'A'&&QByteArrayView{gf.data}.trimmed().startsWith(":Z64:");
  const auto z64Data=z64?decodeZ64(gf,offset,diagnostics):std::optional<QByteArray>{};
  if(z64&&!z64Data)return;
  const auto asciiData=gf.compression==u'A'&&!z64?decodeAsciiGraphic(gf,offset,diagnostics)
    :std::optional<QByteArray>{};
  if(gf.compression==u'A'&&!z64&&!asciiData)return;
  if(gf.compression==u'B'||z64||asciiData){
    const QByteArrayView bytes=z64?QByteArrayView{*z64Data}
      :(asciiData?QByteArrayView{*asciiData}:QByteArrayView{gf.data});
    // SourceOver with fully opaque ink replaces the destination exactly.
    // Clip in wide arithmetic before requesting scanlines; input validation
    // above still processes the entire graphic, even when it is off-label.
    // QColor::alpha() rounds 16-bit alpha, so it cannot decide opacity here.
    if(options.foreground.rgba64().alpha()==65535){
      if(bytes.isEmpty())return;
      const qsizetype rowBytes=gf.bytesPerRow;
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
    const qint64 rowWidth=static_cast<qint64>(gf.bytesPerRow)*8;
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

void drawCode39(QImage& image, State& state, const Barcode& barcode, const QString& data, const RenderOptions& options) {
  const int narrow=std::max(1,state.barcodeDefaults.moduleWidth);
  const int wide=std::max(narrow+1,qRound(narrow*state.barcodeDefaults.wideToNarrowRatio));
  const int height=std::max(1,barcode.parameters.size()>2 && !barcode.parameters[2].isEmpty() ? barcode.parameters[2].toInt() : state.barcodeDefaults.height);
  const QString encoded=u'*'+data.toUpper()+u'*';
  int width=-narrow;
  for(const auto c:encoded)for(const auto element:code39Pattern(c))width+=element==u'w'?wide:narrow;
  width+=static_cast<int>(encoded.size())*narrow;
  if(width<=0)return;
  QImage symbol(width,height,QImage::Format_ARGB32_Premultiplied);symbol.fill(options.background);
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
    case Orientation::Inverted:position.rx()-=width;break;
    case Orientation::BottomUp:position-=QPoint(height,width);break;
  }
  symbol=rotateImage(symbol,barcode.orientation);
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
  int height=state.barcodeDefaults.height>0?state.barcodeDefaults.height:100;
  if(barcode.parameters.size()>1&&!barcode.parameters[1].isEmpty())height=std::max(1,barcode.parameters[1].toInt());
  const bool interpretation=barcode.parameters.size()<=2||barcode.parameters[2].compare(u"N",Qt::CaseInsensitive)!=0;
  const bool interpretationAbove=barcode.parameters.size()>3&&barcode.parameters[3].compare(u"Y",Qt::CaseInsensitive)==0;
  const int guardExtra=interpretation?std::max(1,5*module-2):13;
  const int textHeight=interpretation?std::max(12,12*module):0;
  const int leftPad=interpretation?14*module:0;
  const int barsTop=interpretation&&interpretationAbove?textHeight:0;
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
    QPainter target(&image);target.drawImage(position,rotateImage(symbol,barcode.orientation));
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
  QPainter target(&image);target.drawImage(position,symbol);
}

void drawCodabar(QImage& image,State& state,const Barcode& barcode,const QString& text,const RenderOptions& options,QList<Diagnostic>& diagnostics) {
  const int ratio=std::clamp(qRound(state.barcodeDefaults.wideToNarrowRatio),2,3);
  auto modules=BarcodeEncoders::codabar(text,ratio);
  if(!modules){diagnostics.append({Severity::Error,u"codabar-encode"_s,modules.error(),-1,u"^BK"_s});return;}
  const int module=std::clamp(state.barcodeDefaults.moduleWidth,1,10);
  const int height=barcode.parameters.size()>2&&!barcode.parameters[2].isEmpty()?std::max(1,barcode.parameters[2].toInt()):std::max(1,state.barcodeDefaults.height);
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
  QPainter painter(&image);painter.drawImage(position,rotateImage(original,orientation));
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
  else if (barcode.symbology == u"B3") drawCode39(image,state,barcode,data,options);
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

std::expected<QImage, RenderError> renderLabel(const Label& label, int index, const RenderOptions& options, QList<Diagnostic>& diagnostics,RenderFonts& fonts) {
  const int width=options.width>0?options.width:(label.width()>0?label.width():812);
  const int height=options.height>0?options.height:(label.height()>0?label.height():1218);
  if(width<=0||height<=0||width>100000||height>100000||qint64(width)*height>64*1024*1024) return std::unexpected(RenderError{u"invalid-size"_s,u"Label dimensions are invalid or unsafe."_s,index});
  QImage image(width,height,QImage::Format_ARGB32_Premultiplied); image.fill(options.background);
  State state;
  int labelTop=0;bool mirror=false;
  for(const auto& command:label.commands()){
    if(const auto* top=std::get_if<LabelTop>(&command.payload))labelTop=top->dots;
    if(const auto* value=std::get_if<PrintMirror>(&command.payload))mirror=value->enabled;
  }
  // An explicitly requested canvas centers a narrower/wider ^PW print area.
  // Apply this before ^PO/^PM, as the Labelary canvas does.
  const int printOffset=label.width()>0?int(std::floor((qint64(width)-label.width())/2.0)):0;
  const qint64 homeX=qint64(options.ignoreLabelHome?0:label.homeX())+printOffset;
  const qint64 homeY=qint64(options.ignoreLabelHome?0:label.homeY())+labelTop;

  const auto prepareGraphicPainter=[&](QPainter& painter,LineColor color,int thickness){
    if(state.reverse){painter.setCompositionMode(QPainter::CompositionMode_Difference);painter.setPen(QPen(Qt::white,thickness));painter.setBrush(Qt::white);}
    else{const auto drawColor=commandColor(color,options);painter.setPen(QPen(drawColor,thickness));painter.setBrush(drawColor);}
  };

  for(const auto& command:label.commands()) {
    state.reverse=state.labelReverse||state.fieldReverse;
    std::visit([&](const auto& value){
      using T=std::decay_t<decltype(value)>;
      if constexpr(std::is_same_v<T,FieldOrigin>){state.position=printerPosition(qint64(value.x)+state.labelShift+homeX,qint64(value.y)+homeY);state.baseline=false;state.justification=value.useDefaultJustification?state.defaultJustification:value.justification;}
      else if constexpr(std::is_same_v<T,FieldTypeset>){state.position=printerPosition(value.usePreviousX?state.nextTypeset.x():qint64(value.x)+state.labelShift+homeX,value.usePreviousY?state.nextTypeset.y():qint64(value.y)+homeY);state.baseline=true;state.justification=value.useDefaultJustification?state.defaultJustification:value.justification;}
      else if constexpr(std::is_same_v<T,ScalableFont>) {state.font=value;state.barcodeInterpretationFont=value;}
      else if constexpr(std::is_same_v<T,FieldSeparator>) {state.font=state.defaultFont;state.barcodeInterpretationFont.reset();state.fieldReverse=false;state.parameter={};state.pendingBarcode.reset();state.block.reset();}
      else if constexpr(std::is_same_v<T,ChangeFont>){state.defaultFont={value.font,Orientation::Normal,value.height,value.width};state.font=state.defaultFont;}
      else if constexpr(std::is_same_v<T,FieldDirection>){state.fieldDirection=value.orientation;state.defaultJustification=value.justification;}
      else if constexpr(std::is_same_v<T,FieldReverse>) state.fieldReverse=true;
      else if constexpr(std::is_same_v<T,LabelReverse>) state.labelReverse=value.enabled;
      else if constexpr(std::is_same_v<T,FieldParameter>) state.parameter=value;
      else if constexpr(std::is_same_v<T,FieldBlock>) state.block=value;
      else if constexpr(std::is_same_v<T,FieldEncoding>) { /* Accepted compatibility command; no raster state change. */ }
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
        const int boxWidth=value.width>0?value.width:value.thickness;
        const int boxHeight=value.height>0?value.height:value.thickness;
        const QPoint origin=state.position-QPoint(state.justification==Justification::Right?boxWidth:0,state.baseline?boxHeight:0);
        const auto fillRectangle=QRect(origin,QSize(boxWidth,boxHeight));
        const bool filled=value.thickness*2>=std::min(boxWidth,boxHeight);
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
        QPainter p(&image);p.setRenderHint(QPainter::Antialiasing,false);p.setPen(Qt::NoPen);p.setBrush(state.reverse?Qt::white:commandColor(value.color,options));
        if(state.reverse)p.setCompositionMode(QPainter::CompositionMode_Difference);
        GraphicGeometry::drawEllipse(p,origin,width,height,value.thickness,image.size());state.reverse=false;
      }
      else if constexpr(std::is_same_v<T,GraphicDiagonal>){
        if(value.width<0||value.height<0||value.width>32000||value.height>32000||value.thickness>32000){diagnostics.append({Severity::Error,u"graphic-dimensions"_s,u"Graphic dimensions must be between 0 and 32000 dots."_s,command.offset,command.source});state.reverse=false;return;}
        const QPoint origin=GraphicGeometry::anchoredOrigin(state.position,value.width,value.height,state.justification==Justification::Right,state.baseline);
        QPainter p(&image);p.setPen(Qt::NoPen);p.setBrush(state.reverse?Qt::white:commandColor(value.color,options));
        if(state.reverse)p.setCompositionMode(QPainter::CompositionMode_Difference);
        GraphicGeometry::drawDiagonal(p,origin,value.width,value.height,value.thickness,value.orientation==u'L',image.size());
        state.reverse=false;
      }
      else if constexpr(std::is_same_v<T,GraphicField>) {
        QPoint position=state.position;
        if(state.baseline&&value.bytesPerRow>0)position.ry()-=value.totalBytes/value.bytesPerRow;
        if(state.justification==Justification::Right)position.rx()-=value.bytesPerRow*8;
        if(state.reverse){
          QImage mask(image.size(),QImage::Format_ARGB32_Premultiplied);mask.fill(Qt::transparent);
          auto ink=options;ink.background=Qt::transparent;ink.foreground=Qt::white;
          drawGraphicField(mask,position,value,ink,command.offset,diagnostics);
          QPainter painter(&image);painter.setCompositionMode(QPainter::CompositionMode_Difference);painter.drawImage(0,0,mask);
        }else drawGraphicField(image,position,value,options,command.offset,diagnostics);
      }
    },command.payload);
  }
  image=rotateImage(image,state.printOrientation);
  if(mirror)image.flip(Qt::Horizontal);
  return image;
}

} // namespace

std::expected<RenderResult, RenderError> render(const Document& document, const RenderOptions& options) {
  if(options.dpi!=203&&options.dpi!=300&&options.dpi!=600) return std::unexpected(RenderError{u"invalid-dpi"_s,u"DPI must be 203, 300, or 600."_s,-1});
  RenderResult result; result.diagnostics=document.diagnostics(); result.labels.reserve(document.labels().size());
  RenderFonts fonts;
  for(int i=0;i<document.labels().size();++i){auto image=renderLabel(document.labels()[i],i,options,result.diagnostics,fonts);if(!image)return std::unexpected(image.error());result.labels.append(std::move(*image));}
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
