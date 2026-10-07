#include <QtZpl/qtzpl.hpp>

#include <QtCore/QCommandLineParser>
#include <QtCore/QFile>
#include <QtCore/QTextStream>
#include <QtGui/QGuiApplication>
#include <QtGui/QImage>
#include <QtGui/QPainter>
#include <QtGui/QPen>

using namespace Qt::StringLiterals;

static QRect inkBounds(const QImage& image) {
  QRect bounds;
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      if (image.pixelColor(x, y).value() < 128) {
        bounds |= QRect(x, y, 1, 1);
      }
    }
  }
  return bounds;
}

int main(int argc, char** argv) {
  QGuiApplication application(argc, argv);
  QCommandLineParser parser;
  parser.setApplicationDescription(u"Render ZPL and compare with a Labelary golden PNG."_s);
  parser.addHelpOption();
  parser.addPositionalArgument(u"zpl"_s, u"Input ZPL file."_s);
  parser.addPositionalArgument(u"golden"_s, u"Golden PNG file."_s);
  parser.addPositionalArgument(u"actual"_s, u"Output path for the rendered PNG."_s);
  parser.addPositionalArgument(u"diff"_s, u"Optional output path for a diff overlay PNG."_s);
  QCommandLineOption widthOption({u"w"_s, u"width"_s}, u"Label width in dots."_s, u"dots"_s);
  QCommandLineOption heightOption(u"height"_s, u"Label height in dots."_s, u"dots"_s);
  QCommandLineOption dpiOption({u"d"_s, u"dpi"_s}, u"Render DPI."_s, u"dpi"_s, u"203"_s);
  QCommandLineOption ignoreHomeOption(u"ignore-label-home"_s, u"Ignore ^LH offsets."_s);
  parser.addOption(widthOption);
  parser.addOption(heightOption);
  parser.addOption(dpiOption);
  parser.addOption(ignoreHomeOption);
  parser.process(application);

  const auto positional = parser.positionalArguments();
  if (positional.size() < 3) {
    QTextStream(stderr) << parser.helpText();
    return 2;
  }

  QFile input(positional.at(0));
  if (!input.open(QIODevice::ReadOnly)) {
    QTextStream(stderr) << "Cannot read ZPL: " << input.errorString() << Qt::endl;
    return 2;
  }

  const QImage golden(positional.at(1));
  if (golden.isNull()) {
    QTextStream(stderr) << "Cannot read golden PNG: " << positional.at(1) << Qt::endl;
    return 2;
  }

  QtZpl::RenderOptions options{
    .dpi = parser.value(dpiOption).toInt(),
    .width = parser.isSet(widthOption) ? parser.value(widthOption).toInt() : 0,
    .height = parser.isSet(heightOption) ? parser.value(heightOption).toInt() : 0,
    .ignoreLabelHome = parser.isSet(ignoreHomeOption),
  };

  const QByteArray bytes=input.readAll();
  // UTF-8 is explicit in ^CI28 fixtures. Keep the byte-preserving legacy path
  // for older examples whose barcode fields contain non-UTF-8 bytes.
  const auto zpl=bytes.contains("^CI28")||bytes.startsWith("\xEF\xBB\xBF")
    ?QString::fromUtf8(bytes):QString::fromLatin1(bytes);
  const auto result = QtZpl::render(zpl, {}, options);
  if (!result || result->labels.isEmpty()) {
    QTextStream(stderr) << "Render failed"
                        << (result ? u": no labels"_s : u": "_s + result.error().message) << Qt::endl;
    return 1;
  }

  const QImage actual = result->labels.front();
  if (!actual.save(positional.at(2), "PNG")) {
    QTextStream(stderr) << "Cannot save actual PNG: " << positional.at(2) << Qt::endl;
    return 2;
  }

  QTextStream out(stdout);
  out << "Actual size:  " << actual.size().width() << 'x' << actual.size().height() << Qt::endl;
  out << "Golden size:  " << golden.size().width() << 'x' << golden.size().height() << Qt::endl;

  const QRect actualBounds = inkBounds(actual);
  const QRect goldenBounds = inkBounds(golden);
  out << "Actual ink:   (" << actualBounds.x() << ',' << actualBounds.y() << ' '
      << actualBounds.width() << 'x' << actualBounds.height() << ')' << Qt::endl;
  out << "Golden ink:   (" << goldenBounds.x() << ',' << goldenBounds.y() << ' '
      << goldenBounds.width() << 'x' << goldenBounds.height() << ')' << Qt::endl;

  if (actual.size() != golden.size()) {
    out << "Size mismatch prevents pixel comparison." << Qt::endl;
    return 1;
  }

  qsizetype different = 0;
  qsizetype intersection = 0;
  qsizetype inkUnion = 0;
  QRect diffBounds;
  QImage diff(actual.size(), QImage::Format_RGB32);
  diff.fill(Qt::white);

  for (int y = 0; y < actual.height(); ++y) {
    for (int x = 0; x < actual.width(); ++x) {
      const bool actualInk = actual.pixelColor(x, y).value() < 128;
      const bool goldenInk = golden.pixelColor(x, y).value() < 128;
      intersection += actualInk && goldenInk;
      inkUnion += actualInk || goldenInk;
      if (actualInk != goldenInk) {
        ++different;
        diffBounds |= QRect(x, y, 1, 1);
        diff.setPixelColor(x, y, QColor(255, 0, 0));
      } else if (actualInk) {
        diff.setPixelColor(x, y, QColor(0, 0, 0));
      }
    }
  }

  const double jaccard = inkUnion > 0 ? static_cast<double>(intersection) / static_cast<double>(inkUnion) : 1.0;
  out << "Different pixels: " << different << Qt::endl;
  out << "Ink Jaccard:      " << QString::number(jaccard, 'f', 6) << Qt::endl;
  out << "Diff bounds:      (" << diffBounds.x() << ',' << diffBounds.y() << ' '
      << diffBounds.width() << 'x' << diffBounds.height() << ')' << Qt::endl;

  for (const auto& diagnostic : result->diagnostics) {
    out << "Diagnostic: " << diagnostic.code << ' ' << diagnostic.message << Qt::endl;
  }

  if (positional.size() >= 4) {
    QImage overlay = golden.convertToFormat(QImage::Format_RGB32);
    QPainter painter(&overlay);
    painter.setPen(QPen(QColor(255, 0, 0), 1));
    for (int y = 0; y < actual.height(); ++y) {
      for (int x = 0; x < actual.width(); ++x) {
        const bool actualInk = actual.pixelColor(x, y).value() < 128;
        const bool goldenInk = golden.pixelColor(x, y).value() < 128;
        if (actualInk != goldenInk) {
          painter.drawPoint(x, y);
        }
      }
    }
    if (!overlay.save(positional.at(3), "PNG")) {
      QTextStream(stderr) << "Cannot save diff PNG: " << positional.at(3) << Qt::endl;
      return 2;
    }
    if (!diff.save(positional.at(3) + u"-raw.png"_s, "PNG")) {
      QTextStream(stderr) << "Cannot save raw diff PNG." << Qt::endl;
      return 2;
    }
    out << "Saved diff overlay: " << positional.at(3) << Qt::endl;
  }

  return different == 0 ? 0 : 1;
}
