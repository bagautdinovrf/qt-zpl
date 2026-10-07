#include <QtTest/QTest>

#include "../src/barcode_encoders.hpp"
#include "../src/maxicode_encoder.hpp"
#include "../src/pdf417_encoder.hpp"

#include <array>
#include <barrier>
#include <future>
#include <random>
#include <vector>

namespace {

using namespace Qt::StringLiterals;
namespace Encoders = QtZpl::BarcodeEncoders;

bool isDigit(QChar character)
{
    return character >= u'0' && character <= u'9';
}

// The pre-optimization automatic encoder, deliberately retaining its simple
// suffix scans. This test oracle covers valid Subset B input and a separate
// sentinel for already-normalized GS1 separators; it does not share the new
// scan strategy or production helpers.
QVector<int> referenceAutomatic(QStringView data, bool gs1 = false)
{
    constexpr QChar fnc1Token(0x100);
    const auto digitRun = [data](qsizetype from) {
        qsizetype end = from;
        while (end < data.size() && isDigit(data[end]))
            ++end;
        return end - from;
    };
    char subset = digitRun(0) >= 4 ? 'C' : 'B';
    QVector<int> words{subset == 'C' ? 105 : 104};
    if (gs1)
        words.append(102);
    qsizetype index = 0;
    while (index < data.size()) {
        if (gs1 && data[index] == fnc1Token) {
            words.append(102);
            ++index;
            continue;
        }
        const auto digits = digitRun(index);
        if (subset == 'B' && digits >= 4) {
            if (digits % 2 != 0)
                words.append(data[index++].unicode() - 32);
            words.append(99);
            subset = 'C';
            continue;
        }
        if (subset == 'C') {
            if (digits >= 2) {
                words.append(data[index].digitValue() * 10 + data[index + 1].digitValue());
                index += 2;
                continue;
            }
            words.append(100);
            subset = 'B';
            continue;
        }
        words.append(data[index++].unicode() - 32);
    }
    int checksum = words.front();
    for (qsizetype index = 1; index < words.size(); ++index)
        checksum += static_cast<int>(index) * words[index];
    words.append(checksum % 103);
    return words;
}

QString mixedInput(std::mt19937& random)
{
    constexpr QStringView text = u"ABCDxyz-_ /";
    QString data;
    for (int segment = 0; segment < 10; ++segment) {
        const int digits = static_cast<int>(random() % 65);
        for (int index = 0; index < digits; ++index)
            data.append(QChar(u'0' + static_cast<char16_t>(random() % 10)));
        if (random() % 3 != 0)
            data.append(text[random() % text.size()]);
    }
    if (data.isEmpty())
        data.append(u'A');
    return data;
}

// Preserve the uncached implementation as an independent expected-value path.
// Every expected polynomial is freshly constructed, including under the
// concurrency test, so these tests cannot pre-initialize the production cache.
QVector<int> referencePdf417Ecc(const QVector<int>& data, int securityLevel)
{
    const auto powerMod = [](int base, int exponent) {
        int result = 1;
        while (exponent > 0) {
            if (exponent & 1)
                result = (result * base) % 929;
            base = (base * base) % 929;
            exponent >>= 1;
        }
        return result;
    };
    const int count = 1 << (securityLevel + 1);
    QVector<int> polynomial{1};
    for (int degree = 1; degree <= count; ++degree) {
        const int root = powerMod(3, degree);
        QVector<int> next(polynomial.size() + 1);
        for (qsizetype index = 0; index < polynomial.size(); ++index) {
            next[index] = (next[index] + polynomial[index]) % 929;
            next[index + 1] = (next[index + 1] + 929 - (polynomial[index] * root) % 929) % 929;
        }
        polynomial = std::move(next);
    }
    QVector<int> factors;
    for (int index = count; index >= 1; --index)
        factors.append(polynomial[index]);
    QVector<int> words(count);
    for (const int value : data) {
        const int temporary = (value + words[0]) % 929;
        for (int index = count - 1; index >= 0; --index) {
            const int add = index > 0 ? words[count - index] : 0;
            words[count - 1 - index] = (add + 929 - (temporary * factors[index]) % 929) % 929;
        }
    }
    for (int& word : words) {
        if (word > 0)
            word = 929 - word;
    }
    return words;
}

QVector<int> referenceMaxiCodeEcc(const QVector<int>& data, int symbols)
{
    std::array<int, 64> logarithm{};
    std::array<int, 63> exponent{};
    for (int index = 0, value = 1; index < 63; ++index) {
        exponent[index] = value;
        logarithm[value] = index;
        value <<= 1;
        if (value & 64)
            value ^= 0x43;
    }
    QVector<int> polynomial(symbols + 1);
    polynomial[0] = 1;
    for (int root = 1; root <= symbols; ++root) {
        polynomial[root] = 1;
        for (int index = root - 1; index > 0; --index) {
            if (polynomial[index] != 0)
                polynomial[index] = exponent[(logarithm[polynomial[index]] + root) % 63];
            polynomial[index] ^= polynomial[index - 1];
        }
        polynomial[0] = exponent[(logarithm[polynomial[0]] + root) % 63];
    }
    QVector<int> ecc(symbols);
    for (const int value : data) {
        const int feedback = ecc[symbols - 1] ^ value;
        for (int index = symbols - 1; index > 0; --index) {
            ecc[index] = ecc[index - 1];
            if (feedback != 0 && polynomial[index] != 0)
                ecc[index] ^= exponent[(logarithm[feedback] + logarithm[polynomial[index]]) % 63];
        }
        ecc[0] = feedback != 0 && polynomial[0] != 0
            ? exponent[(logarithm[feedback] + logarithm[polynomial[0]]) % 63] : 0;
    }
    return ecc;
}

QVector<int> withReferenceMaxiCodeParity(QVector<int> words)
{
    const auto primary = referenceMaxiCodeEcc(words.first(10), 10);
    for (int index = 0; index < 10; ++index)
        words[10 + index] = primary[9 - index];
    QVector<int> even, odd;
    for (int index = 0; index < 84; index += 2) {
        even.append(words[20 + index]);
        odd.append(words[21 + index]);
    }
    const auto evenEcc = referenceMaxiCodeEcc(even, 20);
    const auto oddEcc = referenceMaxiCodeEcc(odd, 20);
    for (int index = 0; index < 20; ++index) {
        words[104 + 2 * index] = evenEcc[19 - index];
        words[105 + 2 * index] = oddEcc[19 - index];
    }
    return words;
}

} // namespace

class EncoderPerformanceRegressionTest final : public QObject {
    Q_OBJECT

private slots:
    void concurrentFirstPdf417UseKeepsEveryLevel()
    {
        const QVector<int> input{16, 902, 1, 278, 827, 900, 295, 902,
                                 2, 326, 823, 544, 900, 149, 900, 900};
        std::array<QVector<int>, 9> expected;
        for (int level = 0; level <= 8; ++level)
            expected[level] = referencePdf417Ecc(input, level);

        constexpr int workers = 18;
        std::barrier start(workers);
        std::vector<std::future<QVector<int>>> futures;
        for (int worker = 0; worker < workers; ++worker) {
            const int level = worker % 9;
            futures.push_back(std::async(std::launch::async, [input, level, &start] {
                start.arrive_and_wait();
                return Encoders::Pdf417::errorCorrection(input, level);
            }));
        }
        for (int worker = 0; worker < workers; ++worker)
            QCOMPARE(futures[worker].get(), expected[worker % 9]);
    }

    void pdf417EccAllLevelsKeepCodewords()
    {
        std::mt19937 random(0x50444634U);
        for (const int size : {0, 1, 16, 128}) {
            for (int sample = 0; sample < 3; ++sample) {
                QVector<int> data;
                for (int index = 0; index < size; ++index)
                    data.append(static_cast<int>(random() % 929));
                for (int level = 0; level <= 8; ++level)
                    QCOMPARE(Encoders::Pdf417::errorCorrection(data, level), referencePdf417Ecc(data, level));
            }
        }
    }

    void concurrentFirstMaxiCodeUseKeepsParity()
    {
        QByteArray data = QByteArrayLiteral("000000000000000[)>") + char(0x1e)
            + QByteArrayLiteral("01") + char(0x1d) + QByteArrayLiteral("96TRACK")
            + char(0x1d) + QByteArrayLiteral("UPSN") + char(0x1e)
            + QByteArrayLiteral("07DATA") + char(0x1e) + char(0x04);
        constexpr int workers = 8;
        std::barrier start(workers);
        using Result = std::expected<Encoders::MaxiCode::Symbol, QString>;
        std::vector<std::future<Result>> futures;
        for (int worker = 0; worker < workers; ++worker) {
            futures.push_back(std::async(std::launch::async, [data, &start] {
                start.arrive_and_wait();
                return Encoders::MaxiCode::encodeMode2(data);
            }));
        }
        QVector<int> firstWords;
        QVector<bool> firstModules;
        for (auto& future : futures) {
            const auto result = future.get();
            QVERIFY(result.has_value());
            QCOMPARE(result->codewords.size(), 144);
            QCOMPARE(result->codewords, withReferenceMaxiCodeParity(result->codewords));
            if (firstWords.isEmpty()) {
                firstWords = result->codewords;
                firstModules = result->grid.modules;
            } else {
                QCOMPARE(result->codewords, firstWords);
                QCOMPARE(result->grid.modules, firstModules);
            }
        }
    }

    void numericRunsKeepExactSubsetTransitions()
    {
        // Odd/even runs, 2/3/4 digit boundaries, and all placements relative to
        // printable text are the cases affected by changing look-ahead scans.
        for (int length = 1; length <= 20; ++length) {
            const auto digits = u"1234567890"_s.repeated(2).first(length);
            for (const auto& prefix : {QString{}, u"A"_s, u"AB"_s}) {
                for (const auto& suffix : {QString{}, u"X"_s, u"XY12"_s}) {
                    const auto data = prefix + digits + suffix;
                    const auto result = Encoders::Detail::code128Codewords(data, u'A');
                    QVERIFY2(result.has_value(), qPrintable(data));
                    QCOMPARE(*result, referenceAutomatic(data));
                }
            }
        }
    }

    void seededMixedInputsKeepCodewords()
    {
        std::mt19937 random(0x51545a50U);
        for (int sample = 0; sample < 300; ++sample) {
            const auto data = mixedInput(random);
            const auto result = Encoders::Detail::code128Codewords(data, u'A');
            QVERIFY2(result.has_value(), qPrintable(data));
            QCOMPARE(*result, referenceAutomatic(data));
        }
    }

    void gs1SeparatorsAndPresentationKeepCodewords()
    {
        std::mt19937 random(0x47533132U);
        for (int sample = 0; sample < 200; ++sample) {
            const auto first = mixedInput(random).remove(u' ');
            const auto second = mixedInput(random).remove(u' ');
            const auto source = u">;>8(91) "_s + first + u">8>8(92) "_s + second + u">0"_s;
            const auto normalized = u"91"_s + first + QChar(0x100) + QChar(0x100)
                + u"92"_s + second + u'>';
            const auto result = Encoders::Detail::code128Codewords(source, u'D');
            QVERIFY2(result.has_value(), qPrintable(source));
            QCOMPARE(*result, referenceAutomatic(normalized, true));
        }
    }

    void longNumericRunsMatchStrictSubsetC()
    {
        for (const int size : {128, 512, 2048, 8192}) {
            const auto data = u"12345678"_s.repeated(size / 8);
            const auto automatic = Encoders::Detail::code128Codewords(data, u'A');
            const auto strict = Encoders::Detail::code128Codewords(data, u'C');
            QVERIFY(automatic.has_value());
            QVERIFY(strict.has_value());
            QCOMPARE(*automatic, *strict);
        }
    }
};

QTEST_APPLESS_MAIN(EncoderPerformanceRegressionTest)
#include "test_encoder_performance.moc"
