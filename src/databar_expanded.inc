// Included within databar_encoder.cpp's anonymous namespace. The algorithms
// follow ISO/IEC 24724:2011 sections 7.2.5-7.2.8 and Zint's BSD implementation.
// Copyright and provenance: databar-ZINT-LICENSE.txt.

enum class GeneralMode { Invalid, Numeric, Alphanumeric, Iso646 };
constexpr std::string_view alphaPunctuation = "*,-./";
constexpr std::string_view isoPunctuation = "!\"%&'()*+,-./:;<=>?_ ";

bool asciiDigit(char c) { return c >= '0' && c <= '9'; }
bool asciiUpper(char c) { return c >= 'A' && c <= 'Z'; }
bool asciiLower(char c) { return c >= 'a' && c <= 'z'; }

GeneralMode generalType(char c)
{
    if (asciiDigit(c) || c == '\x1D')
        return GeneralMode::Numeric;
    if (asciiUpper(c) || alphaPunctuation.find(c) != std::string_view::npos)
        return GeneralMode::Alphanumeric;
    if (asciiLower(c) || isoPunctuation.find(c) != std::string_view::npos)
        return GeneralMode::Iso646;
    return GeneralMode::Invalid;
}

void appendBits(QByteArray& bits, int value, int count)
{
    for (int bit = count - 1; bit >= 0; --bit)
        bits.append(value & (1 << bit) ? '1' : '0');
}

struct GeneralResult {
    GeneralMode mode = GeneralMode::Numeric;
    char lastDigit = 0;
};

// Plan complete character runs before writing latches. Labelary retains short
// uppercase runs in ISO646 and switches four-digit runs to Numeric even when
// another ISO646 character follows. This is the run-based interpretation of
// ISO/IEC 24724 section 7.2.5.5 used by OkapiBarcode (Apache-2.0); provenance and
// license are preserved in databar-ZINT-LICENSE.txt.
std::expected<QVector<GeneralMode>, QString> generalModes(QByteArrayView field)
{
    struct Run { GeneralMode mode; int length; };
    std::vector<Run> runs;
    for (const char c : field) {
        auto mode = generalType(c);
        if (mode == GeneralMode::Invalid)
            return std::unexpected(u"GS1 DataBar Expanded contains a character outside GS1's character set"_s);
        if (c == '\x1D' && !runs.empty())
            mode = runs.back().mode;
        if (!runs.empty() && runs.back().mode == mode)
            ++runs.back().length;
        else
            runs.push_back({mode, 1});
    }
    for (size_t i = 0; i + 1 < runs.size(); ++i) {
        const auto mode = runs[i].mode;
        auto& next = runs[i + 1];
        if (mode == GeneralMode::Iso646) {
            if (next.mode == GeneralMode::Numeric && next.length < 4)
                next.mode = GeneralMode::Iso646;
            else if (next.mode == GeneralMode::Alphanumeric && next.length < 5)
                next.mode = GeneralMode::Iso646;
        } else if (mode == GeneralMode::Alphanumeric && next.mode == GeneralMode::Numeric) {
            const bool finalNumericRun = i + 2 == runs.size();
            if (next.length < (finalNumericRun ? 4 : 6))
                next.mode = GeneralMode::Alphanumeric;
        }
    }
    std::vector<Run> merged;
    for (const auto run : runs) {
        if (!merged.empty() && merged.back().mode == run.mode)
            merged.back().length += run.length;
        else
            merged.push_back(run);
    }
    for (size_t i = 0; i + 1 < merged.size(); ++i)
        if (merged[i].mode == GeneralMode::Numeric && merged[i].length % 2) {
            --merged[i].length;
            ++merged[i + 1].length;
        }
    QVector<GeneralMode> result;
    result.reserve(field.size());
    for (const auto run : merged)
        for (int i = 0; i < run.length; ++i)
            result.append(run.mode);
    return result;
}

std::expected<GeneralResult, QString> generalField(QByteArrayView field, QByteArray& bits)
{
    const auto modes = generalModes(field);
    if (!modes)
        return std::unexpected(modes.error());
    GeneralResult result;
    for (qsizetype i = 0; i < field.size();) {
        const char c = field[i];
        const auto mode = (*modes)[i];
        if (mode == GeneralMode::Numeric && i + 1 == field.size()) {
            result.lastDigit = c;
            break;
        }
        if (result.mode != mode) {
            if (mode == GeneralMode::Numeric)
                appendBits(bits, 0, 3);
            else if (mode == GeneralMode::Alphanumeric)
                appendBits(bits, result.mode == GeneralMode::Numeric ? 0 : 4,
                           result.mode == GeneralMode::Numeric ? 4 : 5);
            else {
                if (result.mode == GeneralMode::Numeric)
                    appendBits(bits, 0, 4);
                appendBits(bits, 4, 5);
            }
            result.mode = mode;
        }
        if (mode == GeneralMode::Numeric) {
            const int first = c == '\x1D' ? 10 : c - '0';
            const int second = field[i + 1] == '\x1D' ? 10 : field[i + 1] - '0';
            appendBits(bits, 11 * first + second + 8, 7);
            i += 2;
            continue;
        }
        if (c == '\x1D')
            appendBits(bits, 15, 5);
        else if (asciiDigit(c))
            appendBits(bits, c - 43, 5);
        else if (asciiUpper(c))
            appendBits(bits, c - (mode == GeneralMode::Alphanumeric ? 33 : 1),
                       mode == GeneralMode::Alphanumeric ? 6 : 7);
        else if (asciiLower(c))
            appendBits(bits, c - 7, 7);
        else if (mode == GeneralMode::Alphanumeric)
            appendBits(bits, int(alphaPunctuation.find(c)) + 58, 6);
        else
            appendBits(bits, int(isoPunctuation.find(c)) + 232, 8);
        ++i;
    }
    return result;
}

int decimal(QByteArrayView data, int pos, int count)
{
    if (pos < 0 || pos + count > data.size())
        return -1;
    int result = 0;
    for (int i = pos; i < pos + count; ++i) {
        if (!asciiDigit(data[i]))
            return -1;
        result = result * 10 + data[i] - '0';
    }
    return result;
}

int expandedDate(QByteArrayView data, int pos)
{
    const int year = decimal(data, pos, 2);
    const int month = decimal(data, pos + 2, 2);
    const int day = decimal(data, pos + 4, 2);
    if (year < 0 || month < 1 || month > 12 || day < 0 || day > 31)
        return -1;
    return year * 384 + (month - 1) * 32 + day;
}

std::expected<QByteArray, QString> expandedBits(QByteArrayView data, int columns)
{
    int method = data.size() >= 16 && data.first(2) == QByteArrayView("01") ? 1 : 2;
    if (method == 1) {
        // Compression omits GTIN's check digit; verify it before that omission.
        int sum = 0;
        for (int i = 2; i < 15; ++i) {
            if (!asciiDigit(data[i]))
                return std::unexpected(u"GS1 DataBar Expanded's AI 01 must contain fourteen ASCII digits"_s);
            sum += (data[i] - '0') * (i % 2 ? 1 : 3);
        }
        if (data[15] != '0' + (10 - sum % 10) % 10)
            return std::unexpected(u"GS1 DataBar Expanded's GTIN check digit is invalid"_s);
    }
    if (method == 1 && data.size() >= 20 && data[2] == '9' && data[16] == '3') {
        if (data.size() >= 26 && (data.sliced(17, 2) == QByteArrayView("10")
                                 || data.sliced(17, 2) == QByteArrayView("20"))) {
            const bool metric = data[17] == '1';
            const int weight = decimal(data, 20, 6);
            if (weight >= 0 && weight <= 99999 && asciiDigit(data[19])) {
                if (data.size() == 26) {
                    if (metric && data[19] == '3' && weight <= 32767)
                        method = 3;
                    else if (!metric && ((data[19] == '2' && weight <= 9999)
                                          || (data[19] == '3' && weight <= 22767)))
                        method = 4;
                    else
                        method = metric ? 7 : 8;
                } else if (data.size() == 34 && data[26] == '1'
                    && std::string_view("1357").find(data[27]) != std::string_view::npos
                    && expandedDate(data, 28) >= 0) {
                    method = (metric ? 6 : 7) + data[27] - '0';
                }
            }
        } else if (data[17] == '9' && data[19] >= '0' && data[19] <= '3') {
            if (data[18] == '2')
                method = 5;
            else if (data[18] == '3' && decimal(data, 20, 3) >= 0)
                method = 6;
        }
    }
    QByteArray bits("0"); // No linked composite component.
    int read = 0;
    switch (method) {
    case 1: appendBits(bits, 4, 3); read = 16; break;
    case 2: appendBits(bits, 0, 4); break;
    case 3: case 4: appendBits(bits, method + 1, 4); read = 26; break;
    case 5: appendBits(bits, 0x30, 7); read = 20; break;
    case 6: appendBits(bits, 0x34, 7); read = 23; break;
    default: appendBits(bits, method + 49, 7); read = int(data.size()); break;
    }
    for (int i = 0; i < read; ++i)
        if (!asciiDigit(data[i]))
            return std::unexpected(u"GS1 DataBar Expanded compressed fields require ASCII digits"_s);
    if (method != 2) {
        if (method == 1)
            appendBits(bits, data[2] - '0', 4);
        for (int i = 3; i < 15; i += 3)
            appendBits(bits, decimal(data, i, 3), 10);
        if (method == 3 || method == 4)
            appendBits(bits, decimal(data, 20, 6) + (method == 4 && data[19] == '3' ? 10000 : 0), 15);
        else if (method == 5 || method == 6) {
            appendBits(bits, data[19] - '0', 2);
            if (method == 6)
                appendBits(bits, decimal(data, 20, 3), 10);
        } else if (method >= 7) {
            appendBits(bits, (data[19] - '0') * 100000 + decimal(data, 21, 5), 20);
            appendBits(bits, data.size() == 34 ? expandedDate(data, 28) : 38400, 16);
        }
    }
    auto general = generalField(data.sliced(read), bits);
    if (!general)
        return std::unexpected(general.error());
    const auto characterCount = [columns](qsizetype count) {
        int characters = int((count + 11) / 12) + 1;
        if (columns && characters % (columns * 2) == 1)
            ++characters;
        return std::max(4, characters);
    };
    int characters = characterCount(bits.size());
    int remainder = 12 * (characters - 1) - int(bits.size());
    if (general->lastDigit) {
        const int digit = general->lastDigit == '\x1D' ? 10 : general->lastDigit - '0';
        if (general->mode != GeneralMode::Numeric)
            appendBits(bits, general->lastDigit - 43, 5);
        else if (remainder >= 4 && remainder <= 6)
            appendBits(bits, digit + 1, 4);
        else
            appendBits(bits, digit * 11 + 18, 7);
        characters = characterCount(bits.size());
        remainder = 12 * (characters - 1) - int(bits.size());
    }
    if (characters > 22)
        return std::unexpected(u"GS1 DataBar Expanded exceeds its 21 data character capacity"_s);
    const int required = 12 * (characters - 1);
    if (general->mode == GeneralMode::Numeric && bits.size() < required)
        appendBits(bits, 0, 4);
    while (bits.size() < required)
        appendBits(bits, 4, 5);
    bits.resize(required);
    if (method == 1 || method == 2 || method == 5 || method == 6) {
        const int offset = method == 1 ? 2 : method == 2 ? 3 : 6;
        bits[offset] = characters % 2 ? '1' : '0';
        bits[offset + 1] = characters > 14 ? '1' : '0';
    }
    return bits;
}

std::array<int, 14> expandedWidths(int value)
{
    int group = 0;
    while (group < 4 && value >= dbar_exp_g_sum[group + 1])
        ++group;
    const int relative = value - dbar_exp_g_sum[group];
    return interleavedWidths(relative / dbar_exp_t_even[group], relative % dbar_exp_t_even[group],
        dbar_exp_modules[group], 17 - dbar_exp_modules[group], 4, dbar_exp_widest[group], true);
}

struct ExpandedRow {
    QVector<bool> modules;
    int columns = 0;
    bool special = false;
    bool leftToRight = true;
    bool oddLast = false;
};

QVector<bool> expandedSeparator(const ExpandedRow& row, int width, bool below, int& version2)
{
    QVector<bool> separator(width, false);
    const int special = below ? 0 : int(row.special);
    for (int x = 4 + special; x < row.modules.size() - 4; ++x)
        separator[x] = !row.modules[x];
    int v2 = version2;
    bool space = false;
    for (int j = 0; j < row.columns; ++j) {
        int finder = 49 * j + 19 + special;
        if (!row.leftToRight && row.oddLast && !below)
            finder -= 17;
        const int begin = row.leftToRight ? (v2 ? 2 : 0) : (v2 ? 14 : 12);
        const int end = row.leftToRight ? (v2 ? 14 : 12) : (v2 ? 2 : 0);
        const int step = row.leftToRight ? 1 : -1;
        for (int i = begin;; i += step) {
            const int x = finder + i;
            if (row.modules[x]) {
                separator[x] = false;
                space = false;
            } else {
                separator[x] = !space;
                space = !space;
            }
            if (i == end)
                break;
        }
        v2 = !v2;
    }
    if (below)
        version2 = v2;
    return separator;
}

std::expected<Matrix, QString> expanded(QStringView text, int segments)
{
    if (text.isEmpty() || text.size() > 77)
        return std::unexpected(u"GS1 DataBar Expanded requires 1-77 characters before compaction"_s);
    if (segments != 0 && (segments < 2 || segments > 20 || segments % 2))
        return std::unexpected(u"GS1 DataBar Expanded segment width must be an even number from 2 to 20"_s);
    QByteArray data;
    data.reserve(text.size());
    for (const QChar c : text) {
        if (c.unicode() > 127)
            return std::unexpected(u"GS1 DataBar Expanded requires ASCII GS1 field data"_s);
        data.append(char(c.unicode()));
    }
    const int columns = segments / 2;
    const auto bits = expandedBits(data, columns);
    if (!bits)
        return std::unexpected(bits.error());
    const int dataChars = int(bits->size() / 12);
    const int symbolChars = dataChars + 1;
    std::array<std::array<int, 14>, 21> widths{};
    int checksum = 0;
    for (int i = 0; i < dataChars; ++i) {
        int value = 0;
        for (int j = 0; j < 12; ++j)
            value = (value << 1) | ((*bits)[i * 12 + j] == '1');
        widths[i] = expandedWidths(value);
        const int weightRow = dbar_exp_weight_rows[(dataChars - 2) / 2][i];
        for (int j = 0; j < 8; ++j)
            checksum += widths[i][j] * dbar_exp_checksum_weight[weightRow][j];
    }
    const auto check = expandedWidths(211 * (symbolChars - 4) + checksum % 211);
    const int blocks = (symbolChars + 1) / 2;
    const int count = blocks * 5 + symbolChars * 8 + 4;
    std::array<int, 235> elements{};
    for (int i = 0; i < blocks; ++i) {
        const int finder = dbar_exp_finder_sequence[(symbolChars - 1) / 2 - 1][i] - 1;
        for (int j = 0; j < 5; ++j)
            elements[21 * i + j + 10] = dbar_exp_finder_pattern[finder][j];
    }
    for (int i = 0; i < 8; ++i)
        elements[i + 2] = check[i];
    for (int i = 0; i < dataChars; ++i) {
        const int offset = i % 2 ? ((i - 1) / 2) * 21 + 23 : (i / 2) * 21 + 15;
        for (int j = 0; j < 8; ++j)
            elements[offset + j] = widths[i][i % 2 ? j : 7 - j];
    }
    Matrix result;
    if (!columns) {
        elements[0] = elements[1] = elements[count - 2] = elements[count - 1] = 1;
        const auto row = expand(std::span(elements).first(count));
        result.width = int(row.size());
        appendRows(result, row, 34);
        return result;
    }

    const int rowCount = (blocks + columns - 1) / columns;
    std::vector<ExpandedRow> rows;
    rows.reserve(rowCount);
    int currentBlock = 0;
    for (int rowNumber = 1; rowNumber <= rowCount; ++rowNumber) {
        ExpandedRow row;
        row.columns = std::min(columns, blocks - currentBlock);
        row.special = rowNumber == rowCount && row.columns != columns && rowNumber % 2 == 0
            && columns % 2 == 0 && row.columns % 2;
        row.leftToRight = columns % 2 || rowNumber % 2 || row.special;
        row.oddLast = rowNumber == rowCount && dataChars % 2 == 0;
        std::array<int, 235> sub{};
        sub[0] = row.special ? 2 : 1;
        sub[1] = 1;
        int subCount = 2;
        for (int reader = 0; reader < row.columns; ++reader, ++currentBlock) {
            const int start = 2 + currentBlock * 21;
            for (int j = 0; j < 21; ++j) {
                if (start + j < count) {
                    const int target = row.leftToRight ? j + reader * 21 + 2
                        : (20 - j) + (row.columns - 1 - reader) * 21 + 2;
                    sub[target] = elements[start + j];
                }
                ++subCount;
            }
        }
        sub[subCount++] = 1;
        sub[subCount++] = 1;
        row.modules = expand(std::span(sub).first(subCount), !(rowNumber % 2 || row.special));
        result.width = std::max(result.width, int(row.modules.size()));
        rows.push_back(std::move(row));
    }
    int version2 = 0;
    for (int i = 0; i < rowCount; ++i) {
        if (i) {
            QVector<bool> middle(result.width, false);
            for (int x = 5; x < 49 * columns; x += 2)
                middle[x] = true;
            appendRows(result, middle, 1);
            appendRows(result, expandedSeparator(rows[i], result.width, false, version2), 1);
        }
        auto modules = rows[i].modules;
        modules.resize(result.width);
        appendRows(result, modules, 34);
        if (i + 1 < rowCount)
            appendRows(result, expandedSeparator(rows[i], result.width, true, version2), 1);
    }
    return result;
}
