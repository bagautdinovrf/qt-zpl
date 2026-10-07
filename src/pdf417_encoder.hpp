#pragma once

#include "barcode_encoders.hpp"

namespace QtZpl::BarcodeEncoders::Pdf417 {

struct Symbol {
  Matrix matrix;
  QVector<int> dataCodewords;
  QVector<int> allCodewords;
  int columns = 0;
  int rows = 0;
};

[[nodiscard]] QTZPL_EXPORT std::expected<Symbol,QString> encode(
  const QByteArray& data,int securityLevel,int columns=0,int rows=0,bool truncated=false);
[[nodiscard]] QTZPL_EXPORT std::expected<QVector<int>,QString> highLevelCodewords(const QByteArray& data);
[[nodiscard]] QTZPL_EXPORT QVector<int> errorCorrection(const QVector<int>& data,int securityLevel);

} // namespace QtZpl::BarcodeEncoders::Pdf417
