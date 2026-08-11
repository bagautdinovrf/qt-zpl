#pragma once

#include <expected>
#include <QtCore/QStringView>
#include <QtGui/QColor>
#include <QtGui/QImage>

#include <QtZpl/model.hpp>

namespace QtZpl {

struct ParseOptions { bool preserveUnknownCommands = true; };
struct RenderOptions {
  int dpi = 203;
  QColor foreground = Qt::black;
  QColor background = Qt::white;
  int width = 0;
  int height = 0;
  bool ignoreLabelHome = false;
};

struct ParseError { QString code; QString message; qsizetype offset = -1; };
struct RenderError { QString code; QString message; int labelIndex = -1; };
struct Error {
  enum class Stage { Parse, Render } stage = Stage::Parse;
  QString code;
  QString message;
  qsizetype offset = -1;
  int labelIndex = -1;
};
struct RenderResult { QList<QImage> labels; QList<Diagnostic> diagnostics; };

QTZPL_EXPORT std::expected<Document, ParseError> parse(QStringView zpl, const ParseOptions& options = {});
QTZPL_EXPORT std::expected<RenderResult, RenderError> render(const Document& document, const RenderOptions& options = {});
QTZPL_EXPORT std::expected<RenderResult, Error> render(QStringView zpl, const ParseOptions& parseOptions = {}, const RenderOptions& renderOptions = {});

} // namespace QtZpl
