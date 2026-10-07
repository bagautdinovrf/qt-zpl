#pragma once

#include "barcode_encoders.hpp"

namespace QtZpl::BarcodeEncoders::MaxiCode {

struct Symbol {
  Matrix grid;
  QVector<int> codewords;
};

[[nodiscard]] QTZPL_EXPORT std::expected<Symbol,QString> encodeMode2(const QByteArray& zplData);
[[nodiscard]] QTZPL_EXPORT std::expected<QByteArray,QString> reconstructMode2(const QByteArray& zplData);
// ZPL ^BD modes 2/3 consume the primary prefix followed by the carrier
// envelope. Modes 4/5/6 consume the field bytes verbatim.
[[nodiscard]] std::expected<Symbol,QString> encode(const QByteArray& zplData,
    int mode = 2, int position = 1, int total = 1);

} // namespace QtZpl::BarcodeEncoders::MaxiCode
