#pragma once

#include "barcode_encoders.hpp"

namespace QtZpl::BarcodeEncoders::MicroPdf417 {

// Zebra ^BF modes 0 through 33 select a fixed column/row combination.
// Each matrix row is one symbol row; callers choose its height in printer dots.
[[nodiscard]] std::expected<Matrix, QString> encode(
    const QByteArray& data, int mode = 0);

// Symbol rows of a mode, equal to encode()'s matrix height; 0 if invalid.
[[nodiscard]] int rows(int mode);

} // namespace QtZpl::BarcodeEncoders::MicroPdf417
