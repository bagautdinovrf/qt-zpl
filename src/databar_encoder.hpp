#pragma once

#include "barcode_encoders.hpp"
#include <QtCore/QStringView>

namespace QtZpl::BarcodeEncoders::DataBar {

// Each matrix cell is one X module. Types 1-5 use the fixed ISO heights
// (33, 13, 13, 69, 10), as does Labelary; ^BR height concerns other hosts.
// Types 3 and 4 include the separator rows. Expanded (type 6) uses 34-module
// rows; a nonzero even segments value selects its stacked variant. Composite
// components are rejected. GS1 AIs use their raw digits and ASCII GS separators.
[[nodiscard]] std::expected<Matrix, QString> encode(QStringView data, int type = 1,
                                                   int rowHeight = 25, int segments = 0);

} // namespace QtZpl::BarcodeEncoders::DataBar
