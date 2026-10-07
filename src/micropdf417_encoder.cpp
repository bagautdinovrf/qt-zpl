#include "micropdf417_encoder.hpp"
#include "pdf417_encoder.hpp"

#include <array>

namespace QtZpl::BarcodeEncoders::MicroPdf417 {
namespace {

#include "micropdf417_tables.inc"
#include "pdf417_codeword_patterns.inc"

// The final Zebra mode is the 4 x 4 variant; ISO's table places it before 4 x 6.
constexpr int variantForMode(int mode) {
    return mode == 33 ? 23 : mode >= 23 ? mode + 1 : mode;
}

QVector<int> correction(const QVector<int>& data, int count, int coefficientOffset) {
    // Polynomial division in GF(929). The table lists coefficients constant first;
    // the register holds the highest-degree remainder coefficient first.
    QVector<int> remainder(count);
    for (int value : data) {
        const int leading = (value + remainder.front()) % 929;
        for (int i = 0; i < count; ++i) {
            const int next = i + 1 < count ? remainder[i + 1] : 0;
            const int coefficient = Microcoeffs[coefficientOffset + count - 1 - i];
            remainder[i] = (next + 929 - leading * coefficient % 929) % 929;
        }
    }
    for (int& value : remainder)
        if (value != 0) value = 929 - value;
    return remainder;
}

void appendPattern(Matrix& matrix, int row, int& x, int pattern, int bits) {
    for (int bit = bits - 1; bit >= 0; --bit)
        matrix.modules[row * matrix.width + x++] = (pattern & (1 << bit)) != 0;
}

} // namespace

std::expected<Matrix, QString> encode(const QByteArray& data, int mode) {
    if (mode < 0 || mode > 33)
        return std::unexpected(QStringLiteral("MicroPDF417 mode must be between 0 and 33"));
    if (data.isEmpty())
        return std::unexpected(QStringLiteral("MicroPDF417 data must not be empty"));
    // 366 digits is the maximum possible capacity, even in numeric compaction.
    // Check before compaction to bound work and allocation for hostile fields.
    if (data.size() > 366)
        return std::unexpected(QStringLiteral("MicroPDF417 data exceeds the maximum capacity"));

    auto encoded = Pdf417::highLevelCodewords(data);
    if (!encoded) return std::unexpected(encoded.error());
    QVector<int> words = std::move(*encoded);
    // PDF417 starts in text mode; MicroPDF417 requires an explicit text latch.
    if (words.front() < 900 || words.front() == 913) words.prepend(900);

    const int variant = variantForMode(mode);
    const int columns = MicroVariants[0][variant];
    const int rows = MicroVariants[1][variant];
    const int errorWords = MicroVariants[2][variant];
    const int capacity = columns * rows - errorWords;
    if (words.size() > capacity)
        return std::unexpected(QStringLiteral("MicroPDF417 data does not fit mode %1 (%2 data codewords)")
                                   .arg(mode).arg(capacity));
    while (words.size() < capacity) words.append(900);
    words += correction(words, errorWords, MicroVariants[3][variant]);

    const int width = 17 * columns + (columns >= 3 ? 31 : 21);
    Matrix matrix{width, rows, QVector<bool>(width * rows)};
    for (int row = 0; row < rows; ++row) {
        const int cluster = (RAPTable[3][variant] + row) % 3;
        const int left = (RAPTable[0][variant] - 1 + row) % 52;
        const int centre = (RAPTable[1][variant] - 1 + row) % 52;
        const int right = (RAPTable[2][variant] - 1 + row) % 52;
        int x = 0;
        appendPattern(matrix, row, x, rap_side[left], 10);
        for (int column = 0; column < columns; ++column) {
            if ((columns == 3 && column == 1) || (columns == 4 && column == 2))
                appendPattern(matrix, row, x, rap_centre[centre], 10);
            appendPattern(matrix, row, x, pdf417CodewordPatterns[cluster][words[row * columns + column]], 17);
        }
        appendPattern(matrix, row, x, rap_side[right], 10);
        appendPattern(matrix, row, x, 1, 1);
    }
    return matrix;
}

} // namespace QtZpl::BarcodeEncoders::MicroPdf417
