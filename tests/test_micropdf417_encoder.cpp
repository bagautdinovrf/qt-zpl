#include "../src/micropdf417_encoder.hpp"
#include "../src/pdf417_encoder.hpp"

#include <QtZpl/qtzpl.hpp>
#include <QtCore/QFile>
#include <QtGui/QImage>
#include <QtTest/QTest>

#include <array>

namespace Encoders = QtZpl::BarcodeEncoders;

class MicroPdf417EncoderTests : public QObject {
    Q_OBJECT
private slots:
    void authoritativeModuleVector();
    void allModeGoldens_data();
    void allModeGoldens();
    void compactionGoldens_data();
    void compactionGoldens();
    void rejectsInvalidInput();
    void maximumCapacity();
    void renderedGoldens_data();
    void renderedGoldens();
    void truncatedPdf417PreservesCodewords();
    void pdf417RejectsOversizedSymbols();
};

void MicroPdf417EncoderTests::authoritativeModuleVector() {
    // Independently transcribed from the bitonal Labelary mode-00 fixture.
    // Data CWs: 900,1,89,900; RS CWs: 522,790,436,801,150,873,921.
    constexpr auto expected =
        "11001000101000011000110010011100110101"
        "11101000101111110101011100011110110101"
        "11101100101010001001111000011110010101"
        "11001100101000011000110010011100010101"
        "11011100101110001001100001011000010101"
        "11011110101100001110010111011000110101"
        "11001110101111101110101110011000100101"
        "11101110101111000001010100011100100101"
        "11100110101111110010111011011110100101"
        "11110110101000111011110110011110101101"
        "11110010101001111110101100011110101001";
    const auto symbol = Encoders::MicroPdf417::encode(QByteArrayLiteral("ABC"));
    QVERIFY(symbol.has_value());
    QCOMPARE(symbol->width, 38);
    QCOMPARE(symbol->height, 11);
    for (qsizetype i = 0; i < symbol->modules.size(); ++i)
        QCOMPARE(symbol->modules[i], expected[i] == '1');
}

void MicroPdf417EncoderTests::allModeGoldens_data() {
    QTest::addColumn<int>("mode");
    for (int mode = 0; mode <= 33; ++mode)
        QTest::newRow(qPrintable(QStringLiteral("mode-%1").arg(mode))) << mode;
}

void MicroPdf417EncoderTests::allModeGoldens() {
    QFETCH(int, mode);
    constexpr std::array rows{11,14,17,20,24,28,8,11,14,17,20,23,26,
                             6,8,10,12,15,20,26,32,38,44,6,8,10,12,15,20,26,32,38,44,4};
    constexpr std::array widths{38,55,82,99};
    const int columns = mode < 6 ? 1 : mode < 13 ? 2 : mode < 23 ? 3 : 4;
    const auto symbol = Encoders::MicroPdf417::encode(QByteArrayLiteral("ABC"), mode);
    QVERIFY(symbol.has_value());
    QCOMPARE(symbol->height, rows[mode]);
    QCOMPARE(symbol->width, widths[columns - 1]);
    const QImage golden(QStringLiteral(":/qtzpl/tests/golden/micropdf417/mode-%1-labelary-bitonal.png")
                            .arg(mode, 2, 10, QLatin1Char('0')));
    QVERIFY(!golden.isNull());
    for (int y = 0; y < symbol->height; ++y)
        for (int x = 0; x < symbol->width; ++x)
            QCOMPARE(symbol->at(x, y), qGray(golden.pixel(20 + x, 20 + 2 * y)) == 0);
}

void MicroPdf417EncoderTests::compactionGoldens_data() {
    QTest::addColumn<QString>("name");
    QTest::addColumn<QByteArray>("data");
    QTest::newRow("numeric") << QStringLiteral("numeric") << QByteArrayLiteral("12345678901234567890");
    QTest::newRow("binary") << QStringLiteral("binary") << QByteArray::fromHex("80818283848586");
}

void MicroPdf417EncoderTests::compactionGoldens() {
    QFETCH(QString, name);
    QFETCH(QByteArray, data);
    const auto symbol = Encoders::MicroPdf417::encode(data, 7);
    QVERIFY(symbol.has_value());
    const QImage golden(QStringLiteral(":/qtzpl/tests/golden/micropdf417/%1-labelary-bitonal.png").arg(name));
    QVERIFY(!golden.isNull());
    for (int y = 0; y < symbol->height; ++y)
        for (int x = 0; x < symbol->width; ++x)
            QCOMPARE(symbol->at(x, y), qGray(golden.pixel(20 + 2 * x, 20 + 3 * y)) == 0);
}

void MicroPdf417EncoderTests::rejectsInvalidInput() {
    QVERIFY(!Encoders::MicroPdf417::encode({}));
    QVERIFY(!Encoders::MicroPdf417::encode(QByteArrayLiteral("ABC"), -1));
    QVERIFY(!Encoders::MicroPdf417::encode(QByteArrayLiteral("ABC"), 34));
    QVERIFY(!Encoders::MicroPdf417::encode(QByteArrayLiteral("ABCDEFG"), 0));
    QVERIFY(!Encoders::MicroPdf417::encode(QByteArray(367, '1'), 32));
    QVERIFY(!Encoders::MicroPdf417::encode(QByteArray(151, char(0x80)), 32));
}

void MicroPdf417EncoderTests::maximumCapacity() {
    const auto text = Encoders::MicroPdf417::encode(QByteArray(250, 'A'), 32);
    QVERIFY(text.has_value());
    QCOMPARE(text->width, 99);
    QCOMPARE(text->height, 44);
    QVERIFY(!Encoders::MicroPdf417::encode(QByteArray(251, 'A'), 32));
    QVERIFY(Encoders::MicroPdf417::encode(QByteArray(366, '1'), 32).has_value());
    QVERIFY(Encoders::MicroPdf417::encode(QByteArray(150, char(0x80)), 32).has_value());
}

void MicroPdf417EncoderTests::renderedGoldens_data() {
    QTest::addColumn<QString>("path");
    for (const QString& folder : {QStringLiteral("micropdf417"), QStringLiteral("pdf417-truncated")})
        for (const QChar orientation : QStringLiteral("nrib")) {
            const QString name = folder + QStringLiteral("/rotation-") + orientation;
            QTest::newRow(qPrintable(name)) << name;
        }
}

void MicroPdf417EncoderTests::renderedGoldens() {
    QFETCH(QString, path);
    const QString prefix = QStringLiteral(":/qtzpl/tests/golden/") + path;
    QFile input(prefix + QStringLiteral(".zpl"));
    QVERIFY(input.open(QIODevice::ReadOnly));
    const auto result = QtZpl::render(QString::fromLatin1(input.readAll()));
    QVERIFY(result.has_value());
    QVERIFY(result->diagnostics.isEmpty());
    QCOMPARE(result->labels.size(), 1);
    const QImage golden(prefix + QStringLiteral("-labelary-bitonal.png"));
    QVERIFY(!golden.isNull());
    const QImage& actual = result->labels.front();
    QCOMPARE(actual.size(), golden.size());
    int differences = 0;
    for (int y = 0; y < golden.height(); ++y)
        for (int x = 0; x < golden.width(); ++x)
            differences += (qGray(actual.pixel(x, y)) < 128) != (qGray(golden.pixel(x, y)) < 128);
    QCOMPARE(differences, 0);
}

void MicroPdf417EncoderTests::truncatedPdf417PreservesCodewords() {
    const auto normal = Encoders::Pdf417::encode(QByteArrayLiteral("ABCDEF"), 2, 2, 8);
    const auto compact = Encoders::Pdf417::encode(QByteArrayLiteral("ABCDEF"), 2, 2, 8, true);
    QVERIFY(normal.has_value());
    QVERIFY(compact.has_value());
    QCOMPARE(compact->dataCodewords, normal->dataCodewords);
    QCOMPARE(compact->allCodewords, normal->allCodewords);
    QCOMPARE(compact->matrix.width, 69);
    QCOMPARE(compact->matrix.height, normal->matrix.height);
    for (int y = 0; y < compact->matrix.height; ++y) {
        for (int x = 0; x < compact->matrix.width - 1; ++x)
            QCOMPARE(compact->matrix.at(x, y), normal->matrix.at(x, y));
        QVERIFY(compact->matrix.at(compact->matrix.width - 1, y));
    }
}

void MicroPdf417EncoderTests::pdf417RejectsOversizedSymbols() {
    for (const bool truncated : {false, true}) {
        // 30 x 90 previously let the symbol-length descriptor index beyond
        // the 929-entry pattern table. The standard allows 928 total words.
        QVERIFY(!Encoders::Pdf417::encode(QByteArrayLiteral("ABC"), 0, 30, 90, truncated));
        QVERIFY(!Encoders::Pdf417::encode(QByteArray(2711, '1'), 0, 0, 0, truncated));
        QVERIFY(!Encoders::Pdf417::encode(QByteArray(1854, 'A'), 0, 0, 0, truncated));
        QVERIFY(!Encoders::Pdf417::encode(QByteArrayLiteral("ABC"), 0, -1, 0, truncated));
        QVERIFY(!Encoders::Pdf417::encode(QByteArrayLiteral("ABC"), 0, 0, 2, truncated));
        const auto largest = Encoders::Pdf417::encode(QByteArray(2710, '1'), 0, 16, 58, truncated);
        QVERIFY(largest.has_value());
        QCOMPARE(largest->allCodewords.size(), 928);
        for (const int word : largest->allCodewords) QVERIFY(word >= 0 && word <= 928);
    }
}

QTEST_MAIN(MicroPdf417EncoderTests)
#include "test_micropdf417_encoder.moc"
