#include "benchmark.hpp"

#include <QtCore/QFile>
#include <QtCore/QtMath>
#include <QtGui/QImage>
#include <QtGui/QPainterPath>
#include <QtGui/QRawFont>
#include <QtGui/QTransform>
#include <stdexcept>
#include <utility>

namespace Bench {
namespace {
using namespace Qt::StringLiterals;

// Extracted from Font0Metrics::outline: keep shaping, advances, per-glyph
// transformation, and path assembly identical in the new/reused face probes.
QPainterPath outline(const QRawFont& font, const QString& text) {
  const auto glyphs = font.glyphIndexesForString(text);
  const auto advances = font.advancesForGlyphIndexes(glyphs, QRawFont::UseDesignMetrics);
  constexpr qreal scaleX = 48.0 / 1000.0;
  constexpr qreal scaleY = 60.0 / 1000.0;
  QPainterPath result;
  qreal x = 0;
  for (qsizetype i = 0; i < glyphs.size(); ++i) {
    QTransform transform;
    transform.translate(x, 0);
    transform.scale(scaleX, scaleY);
    result.addPath(transform.map(font.pathForGlyph(glyphs[i])));
    x += advances[i].x() * scaleX;
  }
  return result;
}

quint64 pathResult(const QPainterPath& path) {
  return static_cast<quint64>(path.elementCount())
      + static_cast<quint64>(qRound64(path.boundingRect().width() * 1000));
}

quint64 imageResult(const QImage& image) {
  if (image.isNull()) throw std::runtime_error("Component image allocation failed");
  const auto* first = reinterpret_cast<const QRgb*>(image.constScanLine(0));
  const auto* last = reinterpret_cast<const QRgb*>(image.constScanLine(image.height() - 1));
  return static_cast<quint64>(image.sizeInBytes()) + first[0] + last[image.width() - 1];
}
} // namespace

void addComponentCases(Cases& cases, const QString& fontFile) {
  QFile source(fontFile);
  if (!source.open(QIODevice::ReadOnly))
    throw std::runtime_error((u"Cannot read component font: "_s + fontFile).toStdString());
  const QByteArray bytes = source.readAll();
  const QRawFont reference(bytes, 1000, QFont::PreferNoHinting);
  if (!reference.isValid() || reference.capHeight() <= 0)
    throw std::runtime_error("Component font is invalid or has no cap height");

  cases.push_back({u"component/font0/construct"_s, u"component"_s,
    u"Construct QRawFont from preloaded TTF at 1000 units; query validity and cap height; destroy face."_s,
    bytes.size(), 1, u"font"_s, 0,
    [bytes]() -> quint64 {
      const QRawFont font(bytes, 1000, QFont::PreferNoHinting);
      if (!font.isValid()) throw std::runtime_error("Component font construction failed");
      return static_cast<quint64>(qRound64(font.capHeight()));
    }});

  const std::pair<QString, QString> texts[] = {
    {u"repeated"_s, QString(32, u'A')},
    {u"varied"_s, u"ABCDEFGHIJKLMNOPQRSTUVWXYZ012345"_s}
  };
  for (const auto& [name, text] : texts) {
    const auto glyphs = reference.glyphIndexesForString(text);
    for (const auto glyph : glyphs)
      if (glyph == 0) throw std::runtime_error("Component font is missing a probe glyph");
    const QRawFont fresh(bytes, 1000, QFont::PreferNoHinting);
    const auto expected = outline(reference, text);
    if (expected.isEmpty() || outline(fresh, text) != expected)
      throw std::runtime_error("Component font paths do not match between faces");

    cases.push_back({u"component/font0/outline-new-"_s + name, u"component"_s,
      u"32 glyphs; construct face, compute glyph indices/advances, transform and assemble outlines. No rasterization."_s,
      text.toUtf8().size(), text.size(), u"glyph"_s, 0,
      [bytes, text]() -> quint64 {
        const QRawFont font(bytes, 1000, QFont::PreferNoHinting);
        return pathResult(outline(font, text));
      }});
    cases.push_back({u"component/font0/outline-reused-"_s + name, u"component"_s,
      u"Same 32-glyph outline work; reuse closure-owned QRawFont across iterations. No rasterization."_s,
      text.toUtf8().size(), text.size(), u"glyph"_s, 0,
      [font = QRawFont(bytes, 1000, QFont::PreferNoHinting), text]() -> quint64 {
        return pathResult(outline(font, text));
      }});
  }

  constexpr int width = 812;
  constexpr int height = 1218;
  constexpr qint64 pixelCount = static_cast<qint64>(width) * height;
  cases.push_back({u"component/image/allocate-fill-812x1218"_s, u"component"_s,
    u"Allocate ARGB32 premultiplied image, fill white, sample two pixels, destroy. No label rendering."_s,
    0, pixelCount, u"pixel"_s, 0,
    []() -> quint64 {
      QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
      image.fill(Qt::white);
      return imageResult(image);
    }});
  cases.push_back({u"component/image/refill-812x1218"_s, u"component"_s,
    u"Fill closure-owned ARGB32 premultiplied image white and sample two pixels. No allocation or rendering."_s,
    0, pixelCount, u"pixel"_s, 0,
    [image = QImage(width, height, QImage::Format_ARGB32_Premultiplied)]() mutable -> quint64 {
      image.fill(Qt::white);
      return imageResult(image);
    }});
}
} // namespace Bench
