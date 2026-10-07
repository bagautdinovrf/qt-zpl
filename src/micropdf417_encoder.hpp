#pragma once

#include "barcode_encoders.hpp"

namespace QtZpl::BarcodeEncoders::MicroPdf417 {

// Zebra ^BF modes 0 through 33 select a fixed column/row combination.
// Each matrix row is one symbol row; callers choose its height in printer dots.
[[nodiscard]] std::expected<Matrix, QString> encode(
    const QByteArray& data, int mode = 0);

} // namespace QtZpl::BarcodeEncoders::MicroPdf417
