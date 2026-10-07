// ISO/IEC 24724 DataBar Omnidirectional, Truncated, Stacked and Limited.
// Adapted from Zint's BSD-3-Clause implementation; the full copyright and
// permission notices (including Annex B's BSI notice) are retained in
// databar-ZINT-LICENSE.txt and databar_tables.hpp.
#include "databar_encoder.hpp"
#include "databar_tables.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace QtZpl::BarcodeEncoders::DataBar {
namespace {
using namespace Qt::StringLiterals;
using namespace Tables;

int combinations(int n, int r)
{
    if (r < 0 || r > n)
        return 0;
    int value = 1;
    for (int i = 1; i <= r; ++i)
        value = value * (n - i + 1) / i;
    return value;
}

// Unrank the width compositions in ISO/IEC 24724:2011 Annex B order.
std::array<int, 7> elementWidths(int value, int modules, int elements, int widest,
                                 bool requireNarrow)
{
    std::array<int, 7> widths{};
    int narrowMask = 0;
    for (int bar = 0; bar < elements - 1; ++bar) {
        int width = 1;
        int subset = 0;
        for (narrowMask |= 1 << bar;; ++width, narrowMask &= ~(1 << bar)) {
            subset = combinations(modules - width - 1, elements - bar - 2);
            if (requireNarrow && !narrowMask
                && modules - width - (elements - bar - 1) >= elements - bar - 1)
                subset -= combinations(modules - width - (elements - bar), elements - bar - 2);
            if (elements - bar - 1 > 1) {
                int tooWide = 0;
                for (int max = modules - width - (elements - bar - 2); max > widest; --max)
                    tooWide += combinations(modules - width - max - 1, elements - bar - 3);
                subset -= tooWide * (elements - 1 - bar);
            } else if (modules - width > widest) {
                --subset;
            }
            value -= subset;
            if (value < 0)
                break;
        }
        value += subset;
        modules -= width;
        widths[bar] = width;
    }
    widths[elements - 1] = modules;
    return widths;
}

std::array<int, 14> interleavedWidths(int odd, int even, int oddModules, int evenModules,
                                     int elements, int widest, bool requireNarrow)
{
    const auto bars = elementWidths(odd, oddModules, elements, widest, requireNarrow);
    const auto spaces = elementWidths(even, evenModules, elements, 9 - widest, !requireNarrow);
    std::array<int, 14> widths{};
    for (int i = 0; i < elements; ++i) {
        widths[i * 2] = bars[i];
        widths[i * 2 + 1] = spaces[i];
    }
    return widths;
}

QVector<bool> expand(std::span<const int> widths, bool black = false)
{
    QVector<bool> row;
    for (const auto width : widths) {
        for (int x = 0; x < width; ++x)
            row.append(black);
        black = !black;
    }
    return row;
}

void appendRows(Matrix& matrix, const QVector<bool>& row, int count)
{
    Q_ASSERT(row.size() == matrix.width);
    for (int y = 0; y < count; ++y)
        matrix.modules.append(row);
    matrix.height += count;
}

// Separator finder alternation always runs left to right, even for the lower
// row, whose data characters are read in the reverse direction.
QVector<bool> omniSeparator(const QVector<bool>& row, int finderStart, bool shiftFinder3)
{
    QVector<bool> separator(50, false);
    for (int x = 4; x < 46; ++x)
        separator[x] = !row[x];
    bool black = true;
    for (int x = finderStart; x < finderStart + 13; ++x) {
        if (shiftFinder3) {
            separator[x] = x == finderStart + 10;
        } else if (!row[x]) {
            separator[x] = black;
            black = !black;
        } else {
            separator[x] = false;
            black = true;
        }
    }
    return separator;
}

Matrix omnidirectional(std::uint64_t value, int type)
{
    const int left = int(value / 4537077);
    const int right = int(value % 4537077);
    const std::array<int, 4> characters{left / 1597, left % 1597, right / 1597, right % 1597};
    std::array<std::array<int, 14>, 4> widths{};
    int checksum = 0;
    for (int i = 0; i < 4; ++i) {
        const bool inside = i % 2;
        int group = inside ? 5 : 0;
        const int end = inside ? 8 : 4;
        while (group < end && characters[i] >= dbar_omn_g_sum[group + 1])
            ++group;
        const int relative = characters[i] - dbar_omn_g_sum[group];
        const int quotient = relative / dbar_omn_t_even_odd[group];
        const int remainder = relative % dbar_omn_t_even_odd[group];
        widths[i] = interleavedWidths(inside ? remainder : quotient, inside ? quotient : remainder,
            dbar_omn_modules[group], dbar_omn_modules[group + 9], 4, dbar_omn_widest[group], inside);
        for (int j = 0; j < 8; ++j)
            checksum += dbar_omn_checksum_weight[i][j] * widths[i][j];
    }
    checksum %= 79;
    if (checksum >= 8)
        ++checksum;
    if (checksum >= 72)
        ++checksum;
    const int leftFinder = checksum / 9;
    const int rightFinder = checksum % 9;
    std::array<int, 46> combined{};
    combined[0] = combined[1] = combined[44] = combined[45] = 1;
    for (int i = 0; i < 8; ++i) {
        combined[i + 2] = widths[0][i];
        combined[i + 15] = widths[1][7 - i];
        combined[i + 23] = widths[3][i];
        combined[i + 36] = widths[2][7 - i];
    }
    for (int i = 0; i < 5; ++i) {
        combined[i + 10] = dbar_omn_finder_pattern[leftFinder][i];
        combined[i + 31] = dbar_omn_finder_pattern[rightFinder][4 - i];
    }

    Matrix result;
    if (type <= 2) {
        result.width = 96;
        appendRows(result, expand(combined), type == 1 ? 33 : 13);
        return result;
    }
    result.width = 50;
    auto top = expand(std::span(combined).first(23));
    top.append(true);
    top.append(false);
    QVector<bool> bottom{true, false};
    bottom.append(expand(std::span(combined).subspan(23), true));
    if (type == 3) {
        QVector<bool> separator(50, false);
        for (int x = 1; x < 46; ++x)
            separator[x] = top[x] == bottom[x] ? !top[x] : !separator[x - 1];
        separator[1] = separator[2] = separator[3] = false;
        appendRows(result, top, 5);
        appendRows(result, separator, 1);
        appendRows(result, bottom, 7);
    } else {
        QVector<bool> middle(50, false);
        for (int x = 5; x < 46; x += 2)
            middle[x] = true;
        appendRows(result, top, 33);
        appendRows(result, omniSeparator(top, 18, false), 1);
        appendRows(result, middle, 1);
        appendRows(result, omniSeparator(bottom, 19, rightFinder == 3), 1);
        appendRows(result, bottom, 33);
    }
    return result;
}

Matrix limited(std::uint64_t value)
{
    constexpr std::array<int, 7> sums{0, 183064, 820064, 1000776, 1491021, 1979845, 1996939};
    const std::array<int, 2> pairs{int(value / 2013571), int(value % 2013571)};
    std::array<std::array<int, 14>, 2> widths{};
    int checksum = 0;
    for (int i = 0; i < 2; ++i) {
        int group = 6;
        while (group && pairs[i] < sums[group])
            --group;
        const int relative = pairs[i] - sums[group];
        widths[i] = interleavedWidths(relative / dbar_ltd_t_even[group], relative % dbar_ltd_t_even[group],
            dbar_ltd_modules[group], 26 - dbar_ltd_modules[group], 7, dbar_ltd_widest[group], false);
        for (int j = 0; j < 14; ++j)
            checksum += dbar_ltd_checksum_weight[i][j] * widths[i][j];
    }
    checksum %= 89;
    std::array<int, 47> combined{};
    combined[0] = combined[1] = combined[44] = combined[45] = 1;
    combined[46] = 5;
    for (int i = 0; i < 14; ++i) {
        combined[i + 2] = widths[0][i];
        combined[i + 16] = dbar_ltd_finder_pattern[checksum][i];
        combined[i + 30] = widths[1][i];
    }
    Matrix result;
    result.width = 79;
    appendRows(result, expand(combined), 10);
    return result;
}
#include "databar_expanded.inc"
} // namespace

std::expected<Matrix, QString> encode(QStringView data, int type, int rowHeight, int segments)
{
    Q_UNUSED(rowHeight);
    if (type == 6)
        return expanded(data, segments);
    if (type < 1 || type > 6)
        return std::unexpected(u"GS1 DataBar type %1 is not supported (supported types: 1-6)"_s.arg(type));
    if (data.isEmpty() || data.size() > 14)
        return std::unexpected(u"GS1 DataBar requires 1-13 digits, or 14 digits including a valid GTIN check digit"_s);
    for (const auto ch : data)
        if (ch < u'0' || ch > u'9')
            return std::unexpected(u"GS1 DataBar requires ASCII digits; composite components are not supported"_s);
    if (data.size() == 14) {
        int sum = 0;
        for (int i = 0; i < 13; ++i)
            sum += data[i].digitValue() * (i % 2 ? 1 : 3);
        if ((10 - sum % 10) % 10 != data[13].digitValue())
            return std::unexpected(u"GS1 DataBar GTIN check digit is invalid"_s);
        data = data.first(13);
    }
    std::uint64_t value = 0;
    for (const auto ch : data)
        value = value * 10 + ch.digitValue();
    if (type == 5 && value > 1999999999999ULL)
        return std::unexpected(u"GS1 DataBar Limited requires a GTIN body between 0 and 1999999999999"_s);
    return type == 5 ? limited(value) : omnidirectional(value, type);
}

} // namespace QtZpl::BarcodeEncoders::DataBar
