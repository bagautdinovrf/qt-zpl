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

// Authored rectangular U+002D, measured from original bitonal Labelary PNGs.
// Glyph 16 is verified against the embedded font's SHA/cmap by the metrics
// tests. Keep its independently measured advance separate from this shape.
// See tests/golden/font0-dashes for phase and rotation reference pages.
QRectF font0HyphenBounds(qreal penX,qreal penY,int width,int height,Orientation orientation) {
  constexpr qreal left=342.0/2048,top=-720.0/2048,right=1517.0/2048,bottom=-497.0/2048;
  if(height<=100){
    // Small glyphs are grid-fitted before placement. Rounding pen+edge would
    // incorrectly change the stroke's width at fractional string positions.
    const int x=qRound(penX),y=qRound(penY);
    const int l=x+qRound(left*width),r=x+qRound(right*width);
    const int t=y+qRound(top*height),b=y+qRound(bottom*height);
    return QRectF(l,t,r-l,b-t);
  }
  const auto snap=[](qreal value,bool down) {
    // Nearest-even at 1/64 dot is explicit and independent of the process
    // floating-point rounding mode, including negative half-grid values.
    const qreal scaled=value*64;
    auto lower=static_cast<qint64>(std::floor(scaled));
    const qreal fraction=scaled-lower;
    if(fraction>0.5||(fraction==0.5&&lower%2!=0))++lower;
    const qreal grid=qreal(lower)/64;
    return down?qFloor(grid):qCeil(grid);
  };
  // Large glyph edges are quantized in label coordinates after rotation.
  // Inverting the world-space ceil uses local floor on a reversed axis.
  const bool reverseX=orientation==Orientation::Inverted||orientation==Orientation::BottomUp;
  const bool reverseY=orientation==Orientation::Rotated90||orientation==Orientation::Inverted;
  const int l=snap(penX+left*width,reverseX),r=snap(penX+right*width,reverseX);
  const int t=snap(penY+top*height,reverseY),b=snap(penY+bottom*height,reverseY);
  return QRectF(l,t,r-l,b-t);
}

} // namespace QtZpl::Rendering
