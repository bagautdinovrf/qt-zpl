#pragma once

#include "barcode_encoders.hpp"

namespace QtZpl::BarcodeEncoders::MaxiCode {

struct Symbol {
  Matrix grid;
  QVector<int> codewords;
};

[[nodiscard]] QTZPL_EXPORT std::expected<Symbol,QString> encodeMode2(const QByteArray& zplData);
[[nodiscard]] QTZPL_EXPORT std::expected<QByteArray,QString> reconstructMode2(const QByteArray& zplData);

} // namespace QtZpl::BarcodeEncoders::MaxiCode
