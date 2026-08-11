#pragma once

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QVector>
#include <expected>
#include <QtZpl/export.hpp>

namespace QtZpl::BarcodeEncoders {

struct Matrix {
  int width = 0;
  int height = 0;
  QVector<bool> modules;
  [[nodiscard]] bool at(int x, int y) const { return modules[y * width + x]; }
};

QTZPL_EXPORT std::expected<Matrix, QString> dataMatrix(QByteArray data, bool gs1, int requestedSize = 0);
QTZPL_EXPORT std::expected<QVector<bool>, QString> ean13(QString data, QString* normalized = nullptr);
QTZPL_EXPORT std::expected<QVector<bool>, QString> code128(QString data, QChar mode = u'N');
QTZPL_EXPORT std::expected<Matrix, QString> qrCode(const QByteArray& data, QChar errorCorrection = u'M', int mask = -1);
QTZPL_EXPORT std::expected<QVector<bool>, QString> codabar(QStringView data, int wideToNarrow = 3);

namespace Detail {
QTZPL_EXPORT QByteArray dataMatrixCodewords(const QByteArray& data, bool gs1);
QTZPL_EXPORT std::expected<QVector<int>, QString> code128Codewords(QStringView data, QChar mode = u'N');
}

} // namespace QtZpl::BarcodeEncoders
