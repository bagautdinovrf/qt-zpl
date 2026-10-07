#include "benchmark.hpp"

#include "barcode_encoders.hpp"
#include "maxicode_encoder.hpp"
#include "pdf417_encoder.hpp"

#include <QtCore/QByteArray>
#include <QtCore/QString>
#include <QtCore/QVector>

#include <expected>
#include <stdexcept>
#include <utility>

namespace Bench {
namespace {

namespace Encoders = QtZpl::BarcodeEncoders;
using namespace Qt::StringLiterals;

template <typename T>
const T& valueOrThrow(const std::expected<T, QString>& result)
{
    if (!result)
        throw std::runtime_error(result.error().toStdString());
    return *result;
}

template <typename T>
quint64 summary(const QVector<T>& values)
{
    if (values.isEmpty())
        throw std::runtime_error("Encoder produced no output");
    return static_cast<quint64>(values.size()) * 131
        + static_cast<quint64>(values.front()) * 17
        + static_cast<quint64>(values[values.size() / 2]) * 7
        + static_cast<quint64>(values.back());
}

quint64 summary(const QByteArray& bytes)
{
    if (bytes.isEmpty())
        throw std::runtime_error("Encoder produced no codewords");
    return static_cast<quint64>(bytes.size()) * 131
        + static_cast<unsigned char>(bytes.front()) * 17
        + static_cast<unsigned char>(bytes[bytes.size() / 2]) * 7
        + static_cast<unsigned char>(bytes.back());
}

quint64 summary(const Encoders::Matrix& matrix)
{
    if (matrix.width <= 0 || matrix.height <= 0
        || matrix.modules.size() != matrix.width * matrix.height)
        throw std::runtime_error("Encoder produced an invalid matrix");
    return summary(matrix.modules) + static_cast<quint64>(matrix.width) * 65537
        + static_cast<quint64>(matrix.height);
}

void addCode128(Cases& cases, QString name, QString description, QString data,
                QChar mode, bool highLevel = false)
{
    Case benchmark{
        .name = u"algorithm/code128/"_s + name,
        .category = u"algorithms"_s,
        .description = std::move(description),
        .inputBytes = data.size(), // All inputs here are ASCII; count encoded bytes.
    };
    if (highLevel) {
        benchmark.run = [data, mode] {
            const auto result = Encoders::Detail::code128Codewords(data, mode);
            return summary(valueOrThrow(result));
        };
    } else {
        benchmark.run = [data, mode] {
            const auto result = Encoders::code128(data, mode);
            return summary(valueOrThrow(result));
        };
    }
    cases.push_back(std::move(benchmark));
}

QByteArray maxiCodePayload()
{
    // Same structured carrier envelope as the existing native-vector test.
    QByteArray data = "000000000000000[)>";
    data.append(char(0x1e));
    data += "01";
    data.append(char(0x1d));
    data += "96TRACK";
    data.append(char(0x1d));
    data += "UPSN";
    data.append(char(0x1e));
    data += "07DATA";
    data.append(char(0x1e));
    data.append(char(0x04));
    return data;
}

} // namespace

void addAlgorithmCases(Cases& cases)
{
    const auto ean = u"5901234123457"_s;
    cases.push_back({
        .name = u"algorithm/ean13/full"_s,
        .category = u"algorithms"_s,
        .description = u"EAN-13: validate 13 digits and produce 95 modules"_s,
        .inputBytes = ean.size(),
        .run = [ean] {
            const auto result = Encoders::ean13(ean);
            return summary(valueOrThrow(result));
        },
    });

    const auto shortText = u"QtZpl-ABC123"_s;
    const auto largeText = u"QtZpl-ABC123-xyz/"_s.repeated(64);
    addCode128(cases, u"b-short"_s, u"Subset B: short mixed ASCII, complete modules"_s,
               shortText, u'B');
    addCode128(cases, u"b-large"_s, u"Subset B: 1024 mixed ASCII bytes, complete modules"_s,
               largeText, u'B');
    addCode128(cases, u"auto-mixed-short"_s, u"Automatic subset: short mixed ASCII, complete modules"_s,
               shortText, u'A');
    addCode128(cases, u"auto-mixed-large"_s, u"Automatic subset: 1024 mixed ASCII bytes, complete modules"_s,
               largeText, u'A');
    addCode128(cases, u"gs1-short"_s, u"GS1: GTIN with leading FNC1 and numeric packing"_s,
               u"(01)04601234567893"_s, u'D');
    addCode128(cases, u"gs1-fields"_s, u"GS1: GTIN, dates, lot and serial, with a field separator"_s,
               u"(01)04601234567893(11)261007(17)261014(10)LOT123>8(21)42"_s, u'D');
    for (const int size : {128, 512, 2048, 8192}) {
        const auto digits = u"12345678"_s.repeated(size / 8);
        addCode128(cases, u"auto-numeric-words-%1"_s.arg(size),
                   u"Automatic subset: one uninterrupted numeric run, high-level codewords only"_s,
                   digits, u'A', true);
    }
    addCode128(cases, u"c-numeric-words-8192"_s,
               u"Strict subset C: same 8192 digits as automatic case, high-level codewords only"_s,
               u"12345678"_s.repeated(1024), u'C', true);

    for (const int size : {16, 128, 1024}) {
        const QByteArray data(size, 'A');
        cases.push_back({
            .name = u"algorithm/datamatrix/full-%1"_s.arg(size),
            .category = u"algorithms"_s,
            .description = u"ECC200 ASCII encoding, padding, Reed-Solomon and module placement"_s,
            .inputBytes = data.size(),
            .run = [data] {
                const auto result = Encoders::dataMatrix(data, false);
                return summary(valueOrThrow(result));
            },
        });
    }
    const QByteArray dmData(1024, 'A');
    cases.push_back({
        .name = u"algorithm/datamatrix/highlevel-1024"_s,
        .category = u"algorithms"_s,
        .description = u"ECC200 ASCII codewords only, same data as full-1024"_s,
        .inputBytes = dmData.size(),
        .run = [dmData] { return summary(Encoders::Detail::dataMatrixCodewords(dmData, false)); },
    });

    for (const int size : {24, 1024}) {
        // Lowercase ensures QR byte mode; every mask gets identical source bytes.
        const QByteArray data = QByteArray("qtzpl123").repeated(size / 8);
        for (const int mask : {-1, 0}) {
            cases.push_back({
                .name = u"algorithm/qr/%1-%2"_s.arg(mask < 0 ? u"auto"_s : u"mask0"_s).arg(size),
                .category = u"algorithms"_s,
                .description = mask < 0
                    ? u"QR byte data, ECC M, automatic selection across eight Zebra-scored masks"_s
                    : u"Same QR byte data and ECC M with fixed mask 0"_s,
                .inputBytes = data.size(),
                .run = [data, mask] {
                    const auto result = Encoders::qrCode(data, u'M', mask);
                    return summary(valueOrThrow(result));
                },
            });
        }
    }

    for (const int size : {32, 1024}) {
        const QByteArray data = QByteArray("Pdf417-Abc123!?/ ").repeated(size / 16);
        cases.push_back({
            .name = u"algorithm/pdf417/full-%1"_s.arg(size),
            .category = u"algorithms"_s,
            .description = u"PDF417 mixed text, ECC level 2, automatic rows/columns, full symbol"_s,
            .inputBytes = data.size(),
            .run = [data] {
                const auto result = Encoders::Pdf417::encode(data, 2);
                const auto& symbol = valueOrThrow(result);
                return summary(symbol.matrix) + summary(symbol.allCodewords);
            },
        });
    }
    const auto pdfText = QByteArray("Pdf417-Abc123!?/ ").repeated(64);
    cases.push_back({
        .name = u"algorithm/pdf417/highlevel-text-1024"_s,
        .category = u"algorithms"_s,
        .description = u"PDF417 text compaction only, same data as full-1024"_s,
        .inputBytes = pdfText.size(),
        .run = [pdfText] {
            const auto result = Encoders::Pdf417::highLevelCodewords(pdfText);
            return summary(valueOrThrow(result));
        },
    });
    const auto pdfDigits = QByteArray("12345678").repeated(128);
    cases.push_back({
        .name = u"algorithm/pdf417/highlevel-numeric-1024"_s,
        .category = u"algorithms"_s,
        .description = u"PDF417 numeric compaction only, 1024 digits"_s,
        .inputBytes = pdfDigits.size(),
        .run = [pdfDigits] {
            const auto result = Encoders::Pdf417::highLevelCodewords(pdfDigits);
            return summary(valueOrThrow(result));
        },
    });

    QVector<int> eccData;
    eccData.reserve(256);
    for (int index = 0; index < 256; ++index)
        eccData.append((index * 37) % 929);
    for (const int level : {2, 5, 8}) {
        cases.push_back({
            .name = u"algorithm/pdf417/ecc-level%1"_s.arg(level),
            .category = u"algorithms"_s,
            .description = u"PDF417 Reed-Solomon for the same 256 input codewords; includes generator construction"_s,
            .inputBytes = static_cast<qint64>(eccData.size() * sizeof(int)),
            .run = [eccData, level] {
                const auto result = Encoders::Pdf417::errorCorrection(eccData, level);
                if (result.size() != (1 << (level + 1)))
                    throw std::runtime_error("PDF417 returned an unexpected ECC length");
                return summary(result);
            },
        });
    }

    const auto maxiData = maxiCodePayload();
    cases.push_back({
        .name = u"algorithm/maxicode/mode2"_s,
        .category = u"algorithms"_s,
        .description = u"MaxiCode mode 2 carrier envelope, compaction, ECC and 30x33 module grid"_s,
        .inputBytes = maxiData.size(),
        .run = [maxiData] {
            const auto result = Encoders::MaxiCode::encodeMode2(maxiData);
            const auto& symbol = valueOrThrow(result);
            return summary(symbol.grid) + summary(symbol.codewords);
        },
    });
}

} // namespace Bench
