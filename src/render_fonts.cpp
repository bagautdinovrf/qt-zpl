// Built-in printer fonts and metrics.
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

static void initializeQtZplResources() {
  Q_INIT_RESOURCE(qtzpl_fonts);
}

namespace QtZpl::Rendering {
using namespace Qt::StringLiterals;

namespace {

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

} // namespace

QString fontAFamily() {
  static const QString family=[] {
    initializeQtZplResources();
    return embeddedFontFamily(u":/qtzpl/fonts/font_a.ttf");
  }();
  return family;
}

namespace {

QString ocrBFontFamily() {
  static const QString family=[] {
    initializeQtZplResources();
    return embeddedFontFamily(u":/qtzpl/fonts/ocr_b.ttf");
  }();
  return family;
}

} // namespace

QFont printerFont(const QString& family,int pixelHeight,int stretch) {
  QFont font(family);
  font.setPixelSize(std::max(1,pixelHeight));
  font.setHintingPreference(QFont::PreferFullHinting);
  font.setStyleStrategy(QFont::NoAntialias);
  font.setStretch(std::clamp(stretch,1,400));
  return font;
}

QFont zebraFont(int pixelHeight, int stretch) {
  return printerFont(zebraFontFamily(),pixelHeight,stretch);
}

QFont builtInFont(QChar name,int pixelHeight,int requestedWidth) {
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

namespace {

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

} // namespace

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

Font0GlyphMask RenderFonts::rasterizeFont0Glyph(const QPainterPath& path) {
  // Filling at an integer offset gives the pixels of the same aliased fill
  // inside a whole field, because Qt quantizes to a 1/64-dot grid.
  const QRect bounds=path.boundingRect().toAlignedRect();
  if(bounds.isEmpty())return {};
  const QRect local(QPoint(0,0),bounds.size());
  const QPainterPath placed=path.translated(-bounds.topLeft());
  constexpr qint64 scratchPixels=64*1024;
  if(qint64(bounds.width())*bounds.height()>scratchPixels){
    Font0GlyphMask result{QImage(bounds.size(),QImage::Format_Alpha8),bounds.topLeft()};
    if(result.image.isNull())return {};
    result.image.fill(0);
    QPainter painter(&result.image);painter.setRenderHint(QPainter::Antialiasing,false);
    painter.fillPath(placed,Qt::black);
    return result;
  }
  if(maskScratch_.width()<bounds.width()||maskScratch_.height()<bounds.height()){
    maskPainter_.reset();
    maskScratch_=QImage(std::max(maskScratch_.width(),bounds.width()),
                        std::max(maskScratch_.height(),bounds.height()),QImage::Format_Alpha8);
    if(maskScratch_.isNull())return {};
    maskPainter_.emplace(&maskScratch_);maskPainter_->setRenderHint(QPainter::Antialiasing,false);
  }
  auto& painter=*maskPainter_;
  painter.setCompositionMode(QPainter::CompositionMode_Source);
  painter.fillRect(local,Qt::transparent);
  painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
  painter.fillPath(placed,Qt::black);
  return {maskScratch_.copy(local),bounds.topLeft()};
}

QRgb RenderFonts::font0InkPixel(const QColor& color) {
  // Take the pixel from Qt itself instead of re-deriving its premultiplied
  // rounding of 16-bit colours; a render normally uses one or two colours.
  const quint64 key=color.rgba64();
  if(const auto found=font0InkPixels_.constFind(key);found!=font0InkPixels_.cend())return found.value();
  QImage probe(1,1,QImage::Format_ARGB32_Premultiplied);probe.fill(Qt::transparent);
  {QPainter painter(&probe);painter.fillRect(QRect(0,0,1,1),color);}
  const QRgb pixel=reinterpret_cast<const QRgb*>(probe.constScanLine(0))[0];
  if(font0InkPixels_.size()<64)font0InkPixels_.insert(key,pixel);
  return pixel;
}

void paintFont0Glyphs(QImage& target,QPoint origin,const QList<Font0PlacedGlyph>& glyphs,QRgb ink) {
  Q_ASSERT(target.format()==QImage::Format_ARGB32_Premultiplied);
  for(const auto& glyph:glyphs){
    const auto& mask=glyph.mask.image;
    // Wide arithmetic keeps far off-canvas glyphs from overflowing int.
    const qint64 left=qint64(origin.x())+glyph.pen.x()+glyph.mask.offset.x();
    const qint64 top=qint64(origin.y())+glyph.pen.y()+glyph.mask.offset.y();
    const qint64 firstX=std::max<qint64>(left,0),lastX=std::min<qint64>(left+mask.width(),target.width());
    const qint64 firstY=std::max<qint64>(top,0),lastY=std::min<qint64>(top+mask.height(),target.height());
    for(qint64 y=firstY;y<lastY;++y){
      const uchar* source=mask.constScanLine(int(y-top));
      auto* line=reinterpret_cast<QRgb*>(target.scanLine(int(y)));
      for(qint64 x=firstX;x<lastX;++x)if(source[x-left])line[x]=ink;
    }
  }
}

void observeRaster(const State& state,QPoint position,const QImage& raster,bool logical) {
  if(!state.observation)return;
  if(logical)observeLogical(state,QRect(position,raster.size()));
  const auto bounds=alphaBounds(raster);
  if(!bounds.isEmpty())observePaint(state,bounds.translated(position));
}

void RenderFonts::loadFontARaster() {
  static const QByteArray bytes=[] {
    initializeQtZplResources();QFile file(u":/qtzpl/fonts/font_a_native.ttf"_s);
    return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};
  }();
  fontARaster_.emplace(bytes);
}

const QByteArray& RenderFonts::font0Bytes() {
  static const QByteArray bytes=[] {
    initializeQtZplResources();
    QFile file(u":/qtzpl/fonts/font0.ttf"_s);
    return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};
  }();
  return bytes;
}

namespace {

// Nearest-even at 1/64 dot is explicit and independent of the process
// floating-point rounding mode, including negative half-grid values.
qreal font0Grid64(qreal value) {
  const qreal scaled=value*64;
  auto lower=static_cast<qint64>(std::floor(scaled));
  const qreal fraction=scaled-lower;
  if(fraction>0.5||(fraction==0.5&&lower%2!=0))++lower;
  return qreal(lower)/64;
}

int quarterTurns(Orientation orientation) {
  switch(orientation){
    case Orientation::Rotated90:return 1;
    case Orientation::Inverted:return 2;
    case Orientation::BottomUp:return 3;
    default:return 0;
  }
}

} // namespace

OutputAxes outputAxes(Orientation field,Orientation print,bool mirror) {
  // rotateImage turns the field and later the label in the same direction.
  // Turning by R reverses local Y, I both axes and B local X. A horizontal
  // mirror then reverses whichever local axis became the output X axis.
  const int turns=(quarterTurns(field)+quarterTurns(print))%4;
  OutputAxes axes{turns==2||turns==3,turns==1||turns==2};
  if(mirror)(turns%2?axes.reverseY:axes.reverseX)^=true;
  return axes;
}

// Authored rectangular U+002D, measured from original bitonal Labelary PNGs.
// Glyph 16 is verified against the embedded font's SHA/cmap by the metrics
// tests. Keep its independently measured advance separate from this shape.
// See tests/golden/font0-dashes for phase and rotation reference pages.
QRectF font0HyphenBounds(qreal penX,qreal penY,int width,int height,OutputAxes axes) {
  constexpr qreal left=342.0/2048,top=-720.0/2048,right=1517.0/2048,bottom=-497.0/2048;
  if(height<=font0OutlineThreshold){
    // Small glyphs are grid-fitted before placement. Rounding pen+edge would
    // incorrectly change the stroke's width at fractional string positions.
    const int x=qRound(penX),y=qRound(penY);
    const int l=x+qRound(left*width),r=x+qRound(right*width);
    const int t=y+qRound(top*height),b=y+qRound(bottom*height);
    return QRectF(l,t,r-l,b-t);
  }
  // Large glyph edges are quantized in output coordinates. The field raster
  // is rotated afterwards: output ceil is local floor on a reversed axis.
  const auto snap=[](qreal value,bool down) {
    const qreal grid=font0Grid64(value);
    return down?qFloor(grid):qCeil(grid);
  };
  const int l=snap(penX+left*width,axes.reverseX),r=snap(penX+right*width,axes.reverseX);
  const int t=snap(penY+top*height,axes.reverseY),b=snap(penY+bottom*height,axes.reverseY);
  return QRectF(l,t,r-l,b-t);
}

void sampleFont0OutlineCorners(QPainterPath& path,OutputAxes axes) {
  // Qt's aliased fill rounds to 1/64 dot and lights pixel i when its centre
  // i+0.5 lies in (left,right]. Labelary lights output pixel i when
  // left <= i < right, like the hyphen's ceil edges. On grid coordinates a
  // 31/64 shift turns one rule into the other; a reversed local axis samples
  // the far corner, which is a half-pixel shift in the opposite direction.
  const qreal dx=axes.reverseX?-0.5:31.0/64;
  const qreal dy=axes.reverseY?-0.5:31.0/64;
  for(int i=0;i<path.elementCount();++i){
    const auto element=path.elementAt(i);
    path.setElementPositionAt(i,font0Grid64(element.x)+dx,font0Grid64(element.y)+dy);
  }
}

} // namespace QtZpl::Rendering
