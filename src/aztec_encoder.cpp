/*
 * Copyright 2016 Huy Cuong Nguyen
 * Copyright 2016 ZXing authors
 * Copyright 2014 Robin Stuart (OkapiBarcode high-level encoding)
 * SPDX-License-Identifier: Apache-2.0
 *
 * Placement adapted from ZXing-C++ v2.3.0 AZEncoder.cpp; text compaction
 * adapted from OkapiBarcode AztecCode.java. QtZpl uses its own bounded byte API and local finite-field
 * implementation; no ZXing runtime is required. See
 * third_party/zxing-aztec/NOTICE and LICENSE.
 */
#include "aztec_encoder.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

namespace QtZpl::BarcodeEncoders::Aztec {
namespace {

template <typename T> constexpr int sizeOf(const T& value) {
    return static_cast<int>(std::size(value));
}

class BitArray final {
public:
    int size() const { return sizeOf(bits_); }
    bool get(int index) const { return bits_[index]; }
    void appendBits(unsigned int value, int count) {
        for (int i = count - 1; i >= 0; --i) bits_.push_back(((value >> i) & 1U) != 0);
    }
    void append(const BitArray& other) {
        bits_.insert(bits_.end(), other.bits_.begin(), other.bits_.end());
    }
private:
    std::vector<bool> bits_;
};

constexpr std::array<int,128> characterSets{
        32, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 12, 32, 32, 32, 32, 32, 32,
        32, 32, 32, 32, 32, 32, 32, 4, 4, 4, 4, 4, 23, 8, 8, 8, 8, 8, 8, 8,
        8, 8, 8, 8, 24, 8, 24, 8, 16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 8, 8,
        8, 8, 8, 8, 4, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 8, 4, 8, 4, 4, 4, 2, 2, 2,
        2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
        2, 2, 2, 8, 4, 8, 4, 4
    };
constexpr std::array<int,128> characterValues{
        0, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 300, 14, 15, 16, 17, 18, 19,
        20, 21, 22, 23, 24, 25, 26, 15, 16, 17, 18, 19, 1, 6, 7, 8, 9, 10, 11, 12,
        13, 14, 15, 16, 301, 18, 302, 20, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 21, 22,
        23, 24, 25, 26, 20, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16,
        17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 27, 21, 28, 22, 23, 24, 2, 3, 4,
        5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
        25, 26, 27, 29, 25, 30, 26, 27
    };

// Block selection follows OkapiBarcode's AztecCode (Apache-2.0). Keeping
// adjacent compatible table runs reproduces Labelary's deterministic choice
// among valid Aztec encodings, including short punctuation and binary runs.
// Mode bits: upper=1, lower=2, mixed=4, punctuation=8, digit=16, binary=32.
struct TextToken { int value; int modes; };
struct TextBlock { int modes; int length; };

int tableIndex(int mode) {
    switch (mode) { case 1:return 0;case 2:return 1;case 16:return 2;case 4:return 3;default:return 4; }
}

void latch(BitArray& bits, int from, int to) {
    constexpr int transitions[5][5]{
        {0,(5<<16)|28,(5<<16)|30,(5<<16)|29,(10<<16)|(29<<5)|30},
        {(10<<16)|(29<<5)|29,0,(5<<16)|30,(5<<16)|29,(10<<16)|(29<<5)|30},
        {(4<<16)|14,(9<<16)|(14<<5)|28,0,(9<<16)|(14<<5)|29,(14<<16)|(14<<10)|(29<<5)|30},
        {(5<<16)|29,(5<<16)|28,(10<<16)|(29<<5)|30,0,(5<<16)|30},
        {(5<<16)|31,(10<<16)|(31<<5)|28,(10<<16)|(31<<5)|30,(10<<16)|(31<<5)|29,0}
    };
    const int transition=transitions[tableIndex(from)][tableIndex(to)];
    bits.appendBits(transition&0xffff,transition>>16);
}

BitArray highLevelEncode(const std::string& data) {
    std::vector<TextToken> tokens;
    tokens.reserve(data.size());
    for (int i=0;i<sizeOf(data);++i) {
        const auto c=static_cast<unsigned char>(data[i]);
        const auto next=i+1<sizeOf(data)?static_cast<unsigned char>(data[i+1]):0;
        int pair=0;
        if(c=='\r'&&next=='\n')pair=2;
        else if(c=='.'&&next==' ')pair=3;
        else if(c==','&&next==' ')pair=4;
        else if(c==':'&&next==' ')pair=5;
        if(pair){tokens.push_back({pair,8});++i;}
        else if(c==0||c>=128)tokens.push_back({c,32});
        else tokens.push_back({characterValues[c],characterSets[c]});
    }
    std::vector<TextBlock> blocks;
    for(const auto token:tokens){
        if(!blocks.empty()&&blocks.back().modes==token.modes)++blocks.back().length;
        else blocks.push_back({token.modes,1});
    }
    // First/last blocks prefer the text tables over DIGIT for shared chars.
    for(const int table:{1,2,4,8})if(blocks.front().modes&table)blocks.front().modes=table;
    if(blocks.size()>1){
        for(int i=1;i<sizeOf(blocks);++i)
            if(blocks[i].modes&blocks[i-1].modes)blocks[i].modes&=blocks[i-1].modes;
        for(const int table:{1,2,4,8})if(blocks.back().modes&table)blocks.back().modes=table;
        for(int i=sizeOf(blocks)-2;i>0;--i)
            if(blocks[i].modes&blocks[i+1].modes)blocks[i].modes&=blocks[i+1].modes;
        for(int i=1;i<sizeOf(blocks);++i)
            for(const int table:{8,4,2,1})if(blocks[i].modes&table)blocks[i].modes=table;
        for(int i=0;i+1<sizeOf(blocks);){
            if(blocks[i].modes==blocks[i+1].modes){
                blocks[i].length+=blocks[i+1].length;
                blocks.erase(blocks.begin()+i+1);
            }else ++i;
        }
    }
    int offset=0;
    for(const auto block:blocks){
        const int shift=block.length<3&&block.modes!=32?64:0;
        for(int i=0;i<block.length;++i)tokens[offset++].modes=block.modes+shift;
    }
    if(tokens.front().modes==65)tokens.front().modes=1;
    BitArray bits;
    int current=1;
    for(int i=0;i<sizeOf(tokens);++i){
        const auto token=tokens[i];
        if(token.modes==32){
            if(current==8||current==16){latch(bits,current,1);current=1;}
            int count=1;
            while(i+count<sizeOf(tokens)&&tokens[i+count].modes==32&&count<2078)++count;
            bits.appendBits(31,5);
            if(count>31){bits.appendBits(0,5);bits.appendBits(count-31,11);}
            else bits.appendBits(count,5);
            for(int j=0;j<count;++j)bits.appendBits(tokens[i+j].value,8);
            i+=count-1;
            continue;
        }
        const int target=token.modes&63;
        const bool shift=token.modes>64;
        if(target!=current){
            if(shift&&target==8&&current!=8)bits.appendBits(0,current==16?4:5);
            else if(shift&&target==1&&current==2)bits.appendBits(28,5);
            else if(shift&&target==1&&current==16)bits.appendBits(15,4);
            else {latch(bits,current,target);current=target;}
        }
        int value=token.value;
        if(value==300)value=target==8?1:14;
        else if(value==301)value=target==8?17:12;
        else if(value==302)value=target==8?19:13;
        bits.appendBits(value,target==16?4:5);
    }
    return bits;
}

// Primitive polynomials from ISO/IEC 24778:2008, clauses 7.2 and 7.4.
int primitivePolynomial(int wordSize) {
    switch (wordSize) {
    case 4: return 0x13;
    case 6: return 0x43;
    case 8: return 0x12d;
    case 10: return 0x409;
    default: return 0x1069; // 12-bit codewords.
    }
}

BitArray checkWords(const BitArray& bits, int totalBits, int wordSize) {
    const int fieldSize = 1 << wordSize;
    const int period = fieldSize - 1;
    std::vector<int> exponent(period * 2), logarithm(fieldSize);
    for (int i = 0, value = 1; i < period; ++i) {
        exponent[i] = exponent[i + period] = value;
        logarithm[value] = i;
        value <<= 1;
        if (value & fieldSize) value ^= primitivePolynomial(wordSize);
    }
    const auto multiply = [&](int left, int right) {
        return left && right ? exponent[logarithm[left] + logarithm[right]] : 0;
    };
    const int dataWords = bits.size() / wordSize;
    const int totalWords = totalBits / wordSize;
    const int eccWords = totalWords - dataWords;
    std::vector<int> generator{1};
    for (int i = 1; i <= eccWords; ++i) {
        std::vector<int> next(generator.size() + 1);
        for (int j = 0; j < sizeOf(generator); ++j) {
            next[j] ^= generator[j];
            next[j + 1] ^= multiply(generator[j], exponent[i]);
        }
        generator = std::move(next);
    }
    std::vector<int> words(totalWords);
    for (int i = 0; i < dataWords; ++i)
        for (int j = 0; j < wordSize; ++j)
            words[i] = (words[i] << 1) | int(bits.get(i * wordSize + j));
    auto remainder = words;
    for (int i = 0; i < dataWords; ++i) {
        const int value = remainder[i];
        if (value)
            for (int j = 1; j <= eccWords; ++j)
                remainder[i + j] ^= multiply(generator[j], value);
    }
    BitArray result;
    result.appendBits(0, totalBits % wordSize);
    for (int i = 0; i < totalWords; ++i)
        result.appendBits(i < dataWords ? words[i] : remainder[i], wordSize);
    return result;
}

BitArray stuffBits(const BitArray& bits, int wordSize) {
    BitArray result;
    const int mask = (1 << wordSize) - 2;
    for (int i = 0; i < bits.size(); i += wordSize) {
        int word = 0;
        for (int j = 0; j < wordSize; ++j)
            if (i + j >= bits.size() || bits.get(i + j)) word |= 1 << (wordSize - 1 - j);
        if ((word & mask) == mask) { result.appendBits(word & mask, wordSize); --i; }
        else if ((word & mask) == 0) { result.appendBits(word | 1, wordSize); --i; }
        else result.appendBits(word, wordSize);
    }
    return result;
}

void setModule(Matrix& matrix, int x, int y) { matrix.modules[y * matrix.width + x] = true; }

void drawBullsEye(Matrix& matrix, int center, int size) {
    for (int i = 0; i < size; i += 2)
        for (int j = center - i; j <= center + i; ++j) {
            setModule(matrix, j, center - i);
            setModule(matrix, j, center + i);
            setModule(matrix, center - i, j);
            setModule(matrix, center + i, j);
        }
    setModule(matrix, center - size, center - size);
    setModule(matrix, center - size + 1, center - size);
    setModule(matrix, center - size, center - size + 1);
    setModule(matrix, center + size, center - size);
    setModule(matrix, center + size, center - size + 1);
    setModule(matrix, center + size, center + size - 1);
}

void drawModeMessage(Matrix& matrix, bool compact, const BitArray& mode) {
    const int center = matrix.width / 2;
    for (int i = 0; i < (compact ? 7 : 10); ++i) {
        const int offset = compact ? center - 3 + i : center - 5 + i + i / 5;
        const int radius = compact ? 5 : 7;
        if (mode.get(i)) setModule(matrix, offset, center - radius);
        if (mode.get(i + (compact ? 7 : 10))) setModule(matrix, center + radius, offset);
        if (mode.get((compact ? 20 : 29) - i)) setModule(matrix, offset, center + radius);
        if (mode.get((compact ? 27 : 39) - i)) setModule(matrix, center - radius, offset);
    }
}

int wordSizeForLayers(int layers) {
    return layers <= 2 ? 6 : layers <= 8 ? 8 : layers <= 22 ? 10 : 12;
}
int capacity(int layers, bool compact) { return ((compact ? 88 : 112) + 16 * layers) * layers; }

Matrix place(const BitArray& message, const BitArray& mode, int layers, bool compact) {
    const int baseSize = (compact ? 11 : 14) + layers * 4;
    const int size = compact ? baseSize : baseSize + 1 + 2 * ((baseSize / 2 - 1) / 15);
    Matrix matrix{size, size, QVector<bool>(size * size, false)};
    std::vector<int> map(baseSize);
    if (compact) std::iota(map.begin(), map.end(), 0);
    else for (int i = 0; i < baseSize / 2; ++i) {
        const int offset = i + i / 15;
        map[baseSize / 2 - i - 1] = size / 2 - offset - 1;
        map[baseSize / 2 + i] = size / 2 + offset + 1;
    }
    for (int layer = 0, rowOffset = 0; layer < layers; ++layer) {
        const int rowSize = (layers - layer) * 4 + (compact ? 9 : 12);
        for (int j = 0; j < rowSize; ++j)
            for (int k = 0; k < 2; ++k) {
                const int column = j * 2 + k;
                if (message.get(rowOffset + column))
                    setModule(matrix, map[layer * 2 + k], map[layer * 2 + j]);
                if (message.get(rowOffset + rowSize * 2 + column))
                    setModule(matrix, map[layer * 2 + j], map[baseSize - 1 - layer * 2 - k]);
                if (message.get(rowOffset + rowSize * 4 + column))
                    setModule(matrix, map[baseSize - 1 - layer * 2 - k], map[baseSize - 1 - layer * 2 - j]);
                if (message.get(rowOffset + rowSize * 6 + column))
                    setModule(matrix, map[baseSize - 1 - layer * 2 - j], map[layer * 2 + k]);
            }
        rowOffset += rowSize * 8;
    }
    drawModeMessage(matrix, compact, mode);
    drawBullsEye(matrix, size / 2, compact ? 5 : 7);
    if (!compact)
        for (int i = 0, j = 0; i < baseSize / 2 - 1; i += 15, j += 16)
            for (int k = (size / 2) & 1; k < size; k += 2) {
                setModule(matrix, size / 2 - j, k);
                setModule(matrix, size / 2 + j, k);
                setModule(matrix, k, size / 2 - j);
                setModule(matrix, k, size / 2 + j);
            }
    return matrix;
}

} // namespace

std::expected<Matrix, QString> encode(const QByteArray& data, int size, bool readerInit, int eci) {
    if (data.isEmpty()) return std::unexpected(QStringLiteral("Aztec data is empty"));
    // 3832 is the theoretical digit capacity; reject huge input before running
    // the high-level state search. The final stuffed-bit capacity is stricter.
    if (data.size() > 3832) return std::unexpected(QStringLiteral("Aztec data exceeds maximum capacity"));
    if (eci < 0 || eci > 999999) return std::unexpected(QStringLiteral("Aztec ECI must be 0 through 999999"));
    if (size == 300) {
        if (readerInit || eci) return std::unexpected(QStringLiteral("Aztec runes do not support reader initialization or ECI"));
        if (data.size() > 3 || std::ranges::any_of(data, [](char c) { return c < '0' || c > '9'; }))
            return std::unexpected(QStringLiteral("Aztec rune requires a decimal integer from 0 through 255"));
        const int value = data.toInt();
        if (value > 255) return std::unexpected(QStringLiteral("Aztec rune requires a decimal integer from 0 through 255"));
        BitArray rune;
        rune.appendBits(value, 8);
        const BitArray checked = checkWords(rune, 28, 4);
        BitArray mode;
        for (int i = 0; i < 28; ++i) mode.appendBits(checked.get(i) ^ (i % 2 == 0), 1);
        return place({}, mode, 0, true);
    }
    const bool automatic = size >= 1 && size <= 99;
    bool compact = size >= 101 && size <= 104;
    if (!automatic && !compact && (size < 201 || size > 232))
        return std::unexpected(QStringLiteral("Aztec size must be 1..99, 101..104, 201..232, or 300"));
    int layers = automatic ? 0 : compact ? size - 100 : size - 200;
    if (readerInit && !automatic && ((compact && layers != 1) || layers > 22))
        return std::unexpected(QStringLiteral("Aztec reader initialization requires compact layer 1 or full layers 1..22"));
    BitArray bits;
    if (eci) {
        bits.appendBits(0, 5); // P/S
        bits.appendBits(0, 5); // FLG(n)
        const QByteArray digits = QByteArray::number(eci);
        bits.appendBits(static_cast<unsigned int>(digits.size()), 3);
        for (const char digit : digits) bits.appendBits(digit - '0' + 2, 4);
    }
    bits.append(highLevelEncode(data.toStdString()));
    BitArray stuffed;
    int wordSize = 0;
    int totalBits = 0;
    bool found = false;
    // Labelary applies the percentage to the complete symbol's word capacity,
    // rounding down to whole words; every size keeps at least three ECC words.
    for (int candidate = 0; candidate <= (automatic ? 32 : 0); ++candidate) {
        if (automatic) {
            compact = candidate <= 3;
            layers = compact ? candidate + 1 : candidate;
            if (readerInit && compact && layers > 1) { compact = false; --layers; }
            if (readerInit && layers > 22) continue;
        }
        wordSize = wordSizeForLayers(layers);
        totalBits = capacity(layers, compact);
        stuffed = stuffBits(bits, wordSize);
        const int dataWords = stuffed.size() / wordSize;
        const int totalWords = totalBits / wordSize;
        const int requiredEcc = automatic ? std::max(3,totalWords*size/100) : 3;
        if (compact && dataWords > (readerInit ? 32 : 64)) continue;
        if (!compact && readerInit && dataWords > 1024) continue;
        if (dataWords + requiredEcc <= totalWords) { found = true; break; }
    }
    if (!found) return std::unexpected(QStringLiteral("Aztec data does not fit the requested size and error correction"));
    const int dataWords = stuffed.size() / wordSize;
    BitArray mode;
    mode.appendBits(layers - 1, compact ? 2 : 5);
    mode.appendBits((dataWords - 1) | (readerInit ? (compact ? 32 : 1024) : 0), compact ? 6 : 11);
    mode = checkWords(mode, compact ? 28 : 40, 4);
    return place(checkWords(stuffed, totalBits, wordSize), mode, layers, compact);
}

} // namespace QtZpl::BarcodeEncoders::Aztec
