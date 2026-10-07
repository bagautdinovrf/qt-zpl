#pragma once

#include "barcode_encoders.hpp"

namespace QtZpl::BarcodeEncoders::Aztec {

// ZPL sizes: 1..99 minimum ECC percentage, 101..104 compact layers,
// 201..232 full layers, 300 decimal Aztec rune (0..255).
// Bytes are encoded verbatim; eci=0 omits ECI, otherwise 1..999999.
[[nodiscard]] std::expected<Matrix, QString> encode(const QByteArray& data,
    int size = 23, bool readerInit = false, int eci = 0);

} // namespace QtZpl::BarcodeEncoders::Aztec
