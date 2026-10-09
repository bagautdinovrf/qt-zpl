#include <QtZpl/qtzpl.hpp>

#include <QtCore/QFile>
#include <QtCore/QTextStream>
#include <QtGui/QGuiApplication>

// A small public-API consumer for source editors. It intentionally owns no
// mutable designer model, UUIDs, undo stack, or ZPL generation policy.
int main(int argc, char** argv) {
  QGuiApplication application(argc, argv);
  const auto arguments = application.arguments();
  QTextStream output(stdout), errors(stderr);
  if (arguments.size() < 2 || arguments.size() > 4) {
    errors << "Usage: qtzpl_inspect <UTF-8.zpl> [zero-based-label-index] [preview.png]\n";
    return 2;
  }
  QFile file(arguments[1]);
  if (!file.open(QIODevice::ReadOnly)) { errors << file.errorString() << '\n'; return 1; }
  bool indexOk = true;
  const int index = arguments.size() > 2 ? arguments[2].toInt(&indexOk) : 0;
  if (!indexOk) { errors << "Invalid label index\n"; return 2; }
  const auto document = QtZpl::parse(QString::fromUtf8(file.readAll()));
  if (!document) { errors << document.error().code << ": " << document.error().message << '\n'; return 1; }

  const auto analysis = QtZpl::analyzeLabel(*document, index);
  if (!analysis) { errors << analysis.error().code << ": " << analysis.error().message << '\n'; return 1; }
  output << "Labels: " << document->labels().size() << "; selected: " << index << '\n';
  for (const auto& field : analysis->fields) {
    output << "Field " << field.id << ", UTF-16 span [" << field.sourceSpan.start
           << ", " << field.sourceSpan.length << "], anchor "
           << field.settings.position.x() << ',' << field.settings.position.y()
           << (field.settings.baseline ? " (baseline)" : " (origin)") << '\n';
    const auto& command = document->labels()[index].commands()[field.commandIndex];
    if (const auto* graphic = std::get_if<QtZpl::GraphicField>(&command.payload)) {
      const auto decoded = QtZpl::decodeGraphic(*graphic);
      if (decoded) output << "  Packed bitmap: " << decoded->size.width() << 'x'
                          << decoded->size.height() << ", " << decoded->bytes.size() << " bytes\n";
    }
  }

  const auto rendered = QtZpl::renderLabel(*document, index, {.collectFieldGeometry = true});
  if (!rendered) { errors << rendered.error().code << ": " << rendered.error().message << '\n'; return 1; }
  for (const auto& field : rendered->fields) {
    const auto bounds = field.clippedBounds;
    output << "Field " << field.field.id << ", clipped paint enclosure "
           << bounds.x() << ',' << bounds.y() << ' ' << bounds.width() << 'x' << bounds.height() << '\n';
  }
  for (const auto& diagnostic : rendered->diagnostics)
    errors << diagnostic.code << " @" << diagnostic.offset << ": " << diagnostic.message << '\n';
  if (arguments.size() > 3 && !rendered->image.save(arguments[3])) {
    errors << "Cannot save the preview image\n";
    return 1;
  }
  return 0;
}
