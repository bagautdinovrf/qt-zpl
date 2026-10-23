// Text fields, field blocks and their anchors.
#include "render_internal.hpp"
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

namespace QtZpl::Rendering {
using namespace Qt::StringLiterals;

namespace {

struct BlockLine {
  QString text;
  int indent = 0;
  bool paragraphEnd = false;
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

} // namespace

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

namespace {

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
  {
    // Font 0 writes its glyph images or outlines into the field itself;
    // QFont text is drawn through one painter.
    constexpr bool font0=std::is_same_v<Metrics,Font0Metrics>;
    QPainter fp;
    if constexpr(!font0){
      fp.begin(&field);fp.setRenderHint(QPainter::TextAntialiasing,false);
      fp.setRenderHint(QPainter::Antialiasing,false);
      fp.setPen(state.reverse?options.background:options.foreground);
    }
    const auto paintLine=[&](int x,int y,const QString& line){
      if constexpr(font0)paintText(field,x,y,line);
      else paintText(fp,x,y,line);
    };
    for(qsizetype lineIndex=0;lineIndex<lines.size();++lineIndex){
      const auto& line=lines[lineIndex];
      const int y=baseline+static_cast<int>(lineIndex)*lineAdvance;
      if(block.justification==Justification::Justified&&!line.paragraphEnd){
        const auto words=line.text.split(u' ',Qt::SkipEmptyParts);
        if(words.size()>1){
          int wordsWidth=0;for(const auto& word:words)wordsWidth+=metrics.horizontalAdvance(word);
          const double gap=static_cast<double>(std::max(0,block.width-line.indent-wordsWidth))/(words.size()-1);
          double x=line.indent;for(const auto& word:words){paintLine(qRound(x),y,word);x+=metrics.horizontalAdvance(word)+gap;}
          continue;
        }
      }
      paintLine(logicalAlignmentX(line,block,metrics),y,line.text);
    }
  }

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
  const Font0Metrics metrics(height,width,fonts,state.parameter,orientation,
                            state.outputOrientation,state.outputMirror);
  const QColor color=state.reverse?options.background:options.foreground;
  if(state.block&&state.block->width>0){
    drawBlockTextLayout(image,state,text,options,metrics,orientation,
      [&](QImage& field,int x,int y,const QString& line){metrics.draw(field,x,y,line,color);});
    return;
  }

  // Small text composes cached glyph images; taller text fills its outline.
  const bool glyphImages=metrics.usesGlyphImages();
  const auto glyphs=glyphImages?metrics.glyphImages(text):QList<Font0PlacedGlyph>{};
  const QPainterPath outline=glyphImages?QPainterPath{}:metrics.outline(text);
  QPoint baselinePosition=state.position;
  if(!state.baseline){
    const qreal span=orientation==Orientation::Inverted||orientation==Orientation::BottomUp
      ?metrics.designAdvance(text)+state.parameter.spacing:0;
    const int count=state.parameter.direction==u'V'?int(text.toUcs4().size()):0;
    baselinePosition+=font0OriginBaseline(height,metrics.ascent(),metrics.cellWidth(),count,span,
                                          state.parameter.direction,orientation);
  }
  QRect bounds;
  if(glyphImages)for(const auto& glyph:glyphs)bounds|=QRect(glyph.pen+glyph.mask.offset,glyph.mask.image.size());
  else bounds=outline.boundingRect().toAlignedRect();
  if(bounds.isEmpty()){state.reverse=false;state.block.reset();return;}
  // An opaque, unrotated field wholly on the canvas can be drawn directly,
  // avoiding a temporary image, its clear, and its composition.
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
    if(glyphImages)paintFont0Glyphs(image,baselinePosition,glyphs,fonts.font0InkPixel(color));
    else{
      QPainter painter(&image);painter.setRenderHint(QPainter::Antialiasing,false);
      painter.translate(int(left),int(top));
      painter.fillPath(outline.translated(-bounds.left(),-bounds.top()),color);
    }
    state.reverse=false;state.block.reset();return;
  }
  QImage field(bounds.size(),QImage::Format_ARGB32_Premultiplied);
  field.fill(Qt::transparent);
  if(glyphImages)paintFont0Glyphs(field,-bounds.topLeft(),glyphs,fonts.font0InkPixel(color));
  else{
    QPainter raster(&field);raster.setRenderHint(QPainter::Antialiasing,false);
    raster.fillPath(outline.translated(-bounds.left(),-bounds.top()),color);
  }

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

} // namespace

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
                           const QColor& ink,RenderFonts& fonts,Orientation orientation) {
  const int fontWidth=std::max(1,qRound(width/5.0))*9;
  const int fontHeight=std::max(1,qRound(height/9.0))*9;
  const int turns=orientation==Orientation::Rotated90?1:orientation==Orientation::Inverted?2:orientation==Orientation::BottomUp?3:0;
  const auto glyph=fonts.fontARaster().glyph(PrinterFontMetrics::fontACharacter(character.unicode()),fontWidth,fontHeight,turns);
  if(glyph)paintPrinterGlyph(painter,baseline,*glyph,ink);
}

namespace {

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
  const auto orientation=state.font.orientationOmitted?state.fieldDirection:state.font.orientation;
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

} // namespace

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
  const auto orientation=state.font.orientationOmitted?state.fieldDirection:state.font.orientation;
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

} // namespace QtZpl::Rendering
