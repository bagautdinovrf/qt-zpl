#include "../src/databar_encoder.hpp"
#include <QtZpl/qtzpl.hpp>

#include <QtCore/QFile>
#include <QtGui/QImage>
#include <QtTest/QTest>

using namespace Qt::StringLiterals;
namespace DataBar = QtZpl::BarcodeEncoders::DataBar;

class DataBarEncoderTests : public QObject {
    Q_OBJECT
private slots:
    void knownModules_data();
    void knownModules();
    void stackedSeparators();
    void expandedStackedSeparators();
    void expandedStackedVariants_data();
    void expandedStackedVariants();
    void checkDigitAndValidation();
    void renderMatchesLabelary_data();
    void renderMatchesLabelary();
};

void DataBarEncoderTests::knownModules_data()
{
    QTest::addColumn<QString>("data");
    QTest::addColumn<int>("type");
    QTest::addColumn<int>("height");
    QTest::addColumn<QByteArray>("expected");
    // ISO/IEC 24724:2011 Figures 1, 2, 4, 7 and 8; GS1 General Specifications
    // Figure 5.5.2.1.1-1. Module vectors transcribed in Zint's test_rss.c,
    // independently cross-checked there with BWIPP. See databar-ZINT-LICENSE.txt.
    QTest::newRow("iso-figure-1") << u"2001234567890"_s << 1 << 33
        << QByteArray("010100011101000001001111111000010100110110111110110000010010100101100000000111000110110110001101");
    QTest::newRow("iso-figure-2") << u"0441234567890"_s << 1 << 33
        << QByteArray("010010001000010001000111000000010101000001100110101100100100000101111110000011000010100011100101");
    QTest::newRow("iso-figure-4-truncated") << u"0001234567890"_s << 2 << 13
        << QByteArray("010101001000000001001111111000010111001011011110111001010110000101111111000111001100111101110101");
    QTest::newRow("gs1-general-specification") << u"0950110153001"_s << 1 << 33
        << QByteArray("010000010100000101000111110000010111101101011100100011011101000101100000000111001110110111001101");
    QTest::newRow("iso-figure-7-limited") << u"1501234567890"_s << 5 << 10
        << QByteArray("0100011001100011011010100111010010101101001101001001011000110111001100110100000");
    QTest::newRow("iso-figure-8-limited") << u"0031234567890"_s << 5 << 10
        << QByteArray("0101010000010010001000010111001010110110100101011000001010010010110000010100000");
    QTest::newRow("gs1-expanded-weight") << u"01906141410000153202000150"_s << 6 << 34
        << QByteArray("0101100011001100001011111111000010100100010000111101110011100010100010111100000011100111010111111011010100000100000110001111110000101000000100011010010");
    QTest::newRow("iso-figure-10-expanded-date") << u"0198898765432106320201234515991231"_s << 6 << 34
        << QByteArray("01001000011000110110111111110000101110000110010100011010000001100010101111110000111010011100000010010100111110111001100011111100001011101100000100100100011110010110001011111111001110001101111010000101");
    QTest::newRow("iso-figure-11-expanded-weight") << u"01900123456789083103001750"_s << 6 << 34
        << QByteArray("0101110010000010011011111111000010111000010011000101011110111001100010111100000011100101110001110111011110101111000110001111110000101011000010011111010");
#include "databar_expanded_vectors.inc"
}

void DataBarEncoderTests::knownModules()
{
    QFETCH(QString, data);
    QFETCH(int, type);
    QFETCH(int, height);
    QFETCH(QByteArray, expected);
    const auto symbol = DataBar::encode(data, type);
    QVERIFY2(symbol.has_value(), qPrintable(symbol ? QString() : symbol.error()));
    QCOMPARE(symbol->width, expected.size());
    QCOMPARE(symbol->height, height);
    for (int y = 0; y < symbol->height; ++y)
        for (int x = 0; x < symbol->width; ++x)
            QCOMPARE(symbol->at(x, y), expected[x] == '1');
}

void DataBarEncoderTests::stackedSeparators()
{
    // ISO/IEC 24724:2011 Figure 5 and Figure 6, including every separator module.
    const auto stacked = DataBar::encode(u"0001234567890", 3);
    QVERIFY(stacked);
    QCOMPARE(stacked->width, 50);
    QCOMPARE(stacked->height, 13);
    const QByteArray stackRows[] = {
        "01010100100000000100111111100001011100101101111010",
        "00001010101011111010000000111010100011010010000000",
        "10111001010110000101111111000111001100111101110101"};
    for (int y = 0; y < 13; ++y)
        for (int x = 0; x < 50; ++x)
            QCOMPARE(stacked->at(x, y), stackRows[y < 5 ? 0 : y == 5 ? 1 : 2][x] == '1');

    const auto omni = DataBar::encode(u"0003456789012", 4);
    QVERIFY(omni);
    QCOMPARE(omni->width, 50);
    QCOMPARE(omni->height, 69);
    const QByteArray omniRows[] = {
        "01010100100000000100111110000001010011100110011010",
        "00001011011111111010000001010100101100011001100000",
        "00000101010101010101010101010101010101010101010000",
        "00001000100010111010010101010000111101001101110000",
        "10110111011101000101100000000111000010110010001101"};
    for (int y = 0; y < 69; ++y)
        for (int x = 0; x < 50; ++x)
            QCOMPARE(omni->at(x, y), omniRows[y < 33 ? 0 : y < 36 ? y - 32 : 4][x] == '1');
}

void DataBarEncoderTests::expandedStackedSeparators()
{
    // GS1 General Specifications Figure 5.5.2.3.2-1, including the shortened
    // final row's guard and all three separator rows (Zint/BWIPP vector).
    const auto matrix = DataBar::encode(u"01906141410000153202000150", 6, 25, 4);
    QVERIFY(matrix);
    QCOMPARE(matrix->width, 102);
    QCOMPARE(matrix->height, 71);
    const QByteArray rows[] = {
        "010110001100110000101111111100001010010001000011110111001110001010001011110000001110011101011111101101",
        "000001110011001111010000000010100101101110111100001000110001110101110100001010100001100010100000010000",
        "000001010101010101010101010101010101010101010101010101010101010101010101010101010101010101010101010000",
        "000001011111011111001010000001010010111111011100100000000000000000000000000000000000000000000000000000",
        "001010100000100000110001111110000101000000100011010010000000000000000000000000000000000000000000000000"};
    for (int y = 0; y < matrix->height; ++y)
        for (int x = 0; x < matrix->width; ++x)
            QCOMPARE(matrix->at(x, y), rows[y < 34 ? 0 : y < 37 ? y - 33 : 4][x] == '1');
}

void DataBarEncoderTests::checkDigitAndValidation()
{
    const auto withoutCheck = DataBar::encode(u"1234567890123");
    const auto withCheck = DataBar::encode(u"12345678901231");
    QVERIFY(withoutCheck);
    QVERIFY(withCheck);
    QCOMPARE(withoutCheck->modules, withCheck->modules);
    QVERIFY(!DataBar::encode(u"12345678901234"));
    QVERIFY(!DataBar::encode(u"123456789012345"));
    QVERIFY(!DataBar::encode(u"12345678901x"));
    QVERIFY(!DataBar::encode(u""));
    QVERIFY(!DataBar::encode(u"2000000000000", 5));
    QVERIFY(!DataBar::encode(u"123|composite", 1));
    QVERIFY(!DataBar::encode(u"[01]12345678901231", 6));
    QVERIFY(!DataBar::encode(u"0112345678901234", 6));
    QVERIFY(!DataBar::encode(QString(78, u'1'), 6));
    QVERIFY(!DataBar::encode(QString(70, u'z'), 6));
    QVERIFY(!DataBar::encode(u"10ABC123", 6, 25, 3));
    QVERIFY(!DataBar::encode(u"10ABC123", 6, 25, 22));
    QVERIFY(!DataBar::encode(u"123", 0));
    QVERIFY(!DataBar::encode(u"123", 12));
    QCOMPARE(DataBar::encode(u"1234567890123", 1, 32000)->modules, withoutCheck->modules);
}

void DataBarEncoderTests::expandedStackedVariants_data()
{
    QTest::addColumn<QString>("data");
    QTest::addColumn<int>("segments");
    QTest::addColumn<int>("width");
    QTest::addColumn<int>("encodedRows");
    QTest::addColumn<QByteArray>("modules");
#include "databar_expanded_stacked_vectors.inc"
}

void DataBarEncoderTests::expandedStackedVariants()
{
    QFETCH(QString, data);
    QFETCH(int, segments);
    QFETCH(int, width);
    QFETCH(int, encodedRows);
    QFETCH(QByteArray, modules);
    const auto matrix = DataBar::encode(data, 6, 25, segments);
    QVERIFY2(matrix.has_value(), qPrintable(matrix ? QString() : matrix.error()));
    const int dataRows = (encodedRows + 3) / 4;
    QCOMPARE(matrix->width, width);
    QCOMPARE(matrix->height, dataRows * 34 + (dataRows - 1) * 3);
    for (int y = 0; y < matrix->height; ++y) {
        const int subrow = y % 37;
        const int row = (y / 37) * 4 + (subrow < 34 ? 0 : subrow - 33);
        for (int x = 0; x < matrix->width; ++x)
            QCOMPARE(matrix->at(x, y), modules[row * width + x] == '1');
    }
}

void DataBarEncoderTests::renderMatchesLabelary_data()
{
    QTest::addColumn<QString>("name");
    for (const auto name : {u"type1-m1-h25", u"type1-m3-h90", u"type2-m2-h25", u"type3-m2-h25",
                            u"type4-m2-h25", u"type5-m2-h25", u"expanded-raw", u"expanded-parentheses",
                            u"expanded-stack", u"expanded-general", u"expanded-general-mixed", u"expanded-stack-long"})
        QTest::newRow(qPrintable(QString(name))) << QString(name);
}

void DataBarEncoderTests::renderMatchesLabelary()
{
    QFETCH(QString, name);
    const auto path = u":/qtzpl/tests/golden/databar/"_s + name;
    QFile zpl(path + u".zpl"_s);
    QVERIFY(zpl.open(QIODevice::ReadOnly));
    const QImage golden(path + u"-labelary-bitonal.png"_s);
    QVERIFY(!golden.isNull());
    const auto result = QtZpl::render(QString::fromUtf8(zpl.readAll()));
    QVERIFY(result.has_value());
    QVERIFY(result->diagnostics.isEmpty());
    QCOMPARE(result->labels.size(), 1);
    QCOMPARE(result->labels.front().convertToFormat(QImage::Format_RGB32),
             golden.convertToFormat(QImage::Format_RGB32));
}

QTEST_MAIN(DataBarEncoderTests)
#include "test_databar_encoder.moc"
