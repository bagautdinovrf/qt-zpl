// ZXing reference vector: Apache-2.0; see third_party/zxing-aztec/NOTICE.
#include "../src/aztec_encoder.hpp"
#include <QtGui/QImage>
#include <QtTest/QTest>
#include <limits>

using QtZpl::BarcodeEncoders::Aztec::encode;

class AztecEncoderTest final : public QObject {
    Q_OBJECT
private slots:
    void publishedModuleVector() {
        // Published ZXing-C++ v2.3.0 AZEncoderTest.Rune vector for value25.
        // Rune parity exercises descriptor RS, bit reversal and orientation.
        const auto symbol = encode("25", 300);
        QVERIFY(symbol.has_value());
        QCOMPARE(symbol->width, 11);
        const QByteArray expected =
            "11101100101"
            "11111111111"
            "01000000011"
            "01011111011"
            "01010001010"
            "11010101011"
            "11010001011"
            "11011111010"
            "11000000011"
            "01111111111"
            "00100100000";
        QCOMPARE(expected.size(), symbol->modules.size());
        for (qsizetype i = 0; i < expected.size(); ++i)
            QCOMPARE(symbol->modules[i], expected[i] == '1');
    }

    void labelaryModules_data() {
        QTest::addColumn<QString>("name");
        QTest::addColumn<QByteArray>("data");
        QTest::addColumn<int>("size");
        QTest::addColumn<bool>("readerInit");
        QTest::newRow("automatic") << QStringLiteral("hello") << QByteArray("HELLO WORLD") << 23 << false;
        QTest::newRow("compact") << QStringLiteral("mixed") << QByteArray("This is an example Aztec symbol for Wikipedia.") << 103 << false;
        QTest::newRow("full") << QStringLiteral("full") << QByteArray("Aztec 123456789: native QtZpl") << 204 << false;
        QTest::newRow("rune") << QStringLiteral("rune") << QByteArray("25") << 300 << false;
        QTest::newRow("reader-init") << QStringLiteral("reader-init") << QByteArray("HELLO") << 101 << true;
        QTest::newRow("binary") << QStringLiteral("binary") << QByteArray::fromHex("00017f80ff") << 102 << false;
    }
    void labelaryModules() {
        QFETCH(QString, name); QFETCH(QByteArray, data); QFETCH(int, size); QFETCH(bool, readerInit);
        const auto symbol = encode(data, size, readerInit);
        QVERIFY2(symbol.has_value(), symbol ? "" : qPrintable(symbol.error()));
        const QImage golden(QStringLiteral(":/qtzpl/tests/golden/aztec/%1-labelary-bitonal.png").arg(name));
        QVERIFY2(!golden.isNull(), qPrintable(name));
        for (int y = 0; y < golden.height(); ++y)
            for (int x = 0; x < golden.width(); ++x) {
                const bool inSymbol = x >= 10 && y >= 10 && x < 10 + symbol->width * 3 && y < 10 + symbol->height * 3;
                const bool actual = inSymbol && symbol->at((x - 10) / 3, (y - 10) / 3);
                const bool expected = qGray(golden.pixel(x, y)) < 128;
                if (actual != expected)
                    QFAIL(qPrintable(QStringLiteral("%1 differs at %2,%3").arg(name).arg(x).arg(y)));
            }
    }
    void validation() {
        QVERIFY(!encode({}));
        QVERIFY(!encode("A", 0));
        QVERIFY(!encode("A", std::numeric_limits<int>::min()));
        QVERIFY(!encode("A", std::numeric_limits<int>::max()));
        QVERIFY(!encode("A", 100));
        QVERIFY(!encode("A", 105));
        QVERIFY(!encode("A", 200));
        QVERIFY(!encode("A", 233));
        QVERIFY(!encode("256", 300));
        QVERIFY(!encode("-1", 300));
        QVERIFY(!encode("25x", 300));
        QVERIFY(!encode("25", 300, true));
        QVERIFY(!encode("HELLO", 102, true));
        QVERIFY(!encode("HELLO", 223, true));
        QVERIFY(!encode("HELLO", 23, false, -1));
        QVERIFY(!encode(QByteArray(4000, 'X')));
        QVERIFY(!encode(QByteArray(100, 'X'), 101));
        QVERIFY(encode(QByteArray(100, '\x80'), 23));
        QVERIFY(encode("HELLO", 201, true));
        QVERIFY(encode("HELLO", 23, false, 26));
    }
    void automaticCorrectionPercentage() {
        // Labelary 8 dpmm probe: these are symbol module widths, independent
        // of magnification; 99% must not be interpreted as 99% of data alone.
        for(const auto [percent,width]:{std::pair{23,15},std::pair{33,15},std::pair{50,19},std::pair{99,79}}){
            const auto symbol=encode("HELLO WORLD",percent);
            QVERIFY(symbol.has_value());QCOMPARE(symbol->width,width);
        }
    }
};

QTEST_GUILESS_MAIN(AztecEncoderTest)
#include "test_aztec_encoder.moc"
