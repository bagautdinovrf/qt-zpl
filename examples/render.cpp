#include <QtZpl/qtzpl.hpp>

#include <QtCore/QDebug>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileInfo>
#include <QtCore/QTextStream>
#include <QtGui/QGuiApplication>

using namespace Qt::StringLiterals;

int main(int argc, char** argv) {
  QGuiApplication application(argc, argv);
  if (application.arguments().size() != 3) {
    qCritical().noquote() << "Usage: qtzpl_render <input.zpl> <output-directory>";
    return 2;
  }

  QFile input(application.arguments().at(1));
  if (!input.open(QIODevice::ReadOnly)) {
    qCritical().noquote() << "Cannot read ZPL file:" << input.errorString();
    return 2;
  }

  const auto zpl = QString::fromUtf8(input.readAll());
  auto result = QtZpl::render(zpl);
  if (!result) {
    qCritical().noquote() << result.error().code + u":"_s << result.error().message;
    return 1;
  }

  for (const auto& diagnostic : result->diagnostics) {
    qWarning().noquote() << diagnostic.code + u":"_s << diagnostic.message;
  }

  QDir outputDirectory(application.arguments().at(2));
  if (!outputDirectory.mkpath(u"."_s)) {
    qCritical().noquote() << "Cannot create output directory:" << outputDirectory.absolutePath();
    return 2;
  }

  for (qsizetype index = 0; index < result->labels.size(); ++index) {
    const auto fileName = u"label_%1.png"_s.arg(index + 1);
    const auto path = outputDirectory.filePath(fileName);
    if (!result->labels.at(index).save(path, "PNG")) {
      qCritical().noquote() << "Cannot save image:" << path;
      return 1;
    }
    QTextStream(stdout) << "Saved: " << QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath())
                        << Qt::endl;
  }

  if (result->labels.isEmpty()) {
    qWarning().noquote() << "No ^XA...^XZ labels found in ZPL.";
  }
  return 0;
}
