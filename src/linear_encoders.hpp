#pragma once
#include <QtCore/QStringView>
#include <QtCore/QVector>
#include <expected>

namespace QtZpl::BarcodeEncoders::Linear {
struct Symbol {
  QVector<bool> modules;
  QString text;
  QString checkText;
};
// Widths are integer printer dots for 2-of-5, and unit modules otherwise.
[[nodiscard]] std::expected<Symbol,QString> encode(QStringView symbology,QStringView data,
  int narrow=1,int wide=3,bool checkDigit=false);
}
