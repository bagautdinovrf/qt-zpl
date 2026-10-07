#include "../src/barcode_encoders.hpp"
#include <QtZpl/qtzpl.hpp>

#include <QtCore/QFile>
#include <QtGui/QImage>
#include <QtTest/QTest>

using namespace Qt::StringLiterals;
namespace Encoders = QtZpl::BarcodeEncoders;

class DataMatrixRectangularTests : public QObject {
    Q_OBJECT
private slots:
    void modulesMatchLabelary_data();
    void modulesMatchLabelary();
    void renderMatchesLabelary_data();
    void renderMatchesLabelary();
    void sizeSelectionAndCapacity();
};

void DataMatrixRectangularTests::modulesMatchLabelary_data()
{
    QTest::addColumn<int>("rows");
    QTest::addColumn<int>("columns");
    QTest::newRow("8x18") << 8 << 18;
    QTest::newRow("8x32") << 8 << 32;
    QTest::newRow("12x26") << 12 << 26;
    QTest::newRow("12x36") << 12 << 36;
    QTest::newRow("16x36") << 16 << 36;
    QTest::newRow("16x48") << 16 << 48;
}

void DataMatrixRectangularTests::modulesMatchLabelary()
{
    QFETCH(int, rows);
    QFETCH(int, columns);
    const auto matrix = Encoders::dataMatrix("123456", false, rows, columns, true);
    QVERIFY2(matrix.has_value(), qPrintable(matrix ? QString() : matrix.error()));
    QCOMPARE(matrix->width, columns);
    QCOMPARE(matrix->height, rows);
    const QImage golden(u":/qtzpl/tests/golden/datamatrix-rectangular/%1x%2-n-labelary-bitonal.png"_s
        .arg(rows).arg(columns));
    QVERIFY(!golden.isNull());
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < columns; ++x)
            QCOMPARE(matrix->at(x, y), qGray(golden.pixel(60 + x * 3, 60 + y * 3)) == 0);
}

void DataMatrixRectangularTests::renderMatchesLabelary_data()
{
    QTest::addColumn<QString>("name");
    for (const auto name : {u"8x18-n", u"8x32-n", u"12x26-n", u"12x36-n", u"16x36-n", u"16x48-n",
                            u"12x26-r", u"12x26-i", u"12x26-b", u"0x0-n"})
        QTest::newRow(qPrintable(QString(name))) << QString(name);
}

void DataMatrixRectangularTests::renderMatchesLabelary()
{
    QFETCH(QString, name);
    const auto path = u":/qtzpl/tests/golden/datamatrix-rectangular/"_s + name;
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

void DataMatrixRectangularTests::sizeSelectionAndCapacity()
{
    const auto automatic = Encoders::dataMatrix("123456", false, 0, 0, true);
    QVERIFY(automatic);
    QCOMPARE(automatic->width, 18);
    QCOMPARE(automatic->height, 8);
    // Numeric pairs consume one codeword. These cross every ECC200 rectangle's
    // capacity boundary; region borders and Reed-Solomon block lengths differ.
    const int capacities[]{5, 10, 16, 22, 32, 49};
    const int rowCounts[]{8, 8, 12, 12, 16, 16};
    const int columnCounts[]{18, 32, 26, 36, 36, 48};
    for (int i = 0; i < 6; ++i) {
        const auto full = Encoders::dataMatrix(QByteArray(capacities[i] * 2, '1'), false,
                                              rowCounts[i], columnCounts[i], true);
        QVERIFY(full);
        QVERIFY(!Encoders::dataMatrix(QByteArray(capacities[i] * 2 + 1, '1'), false,
                                      rowCounts[i], columnCounts[i], true));
        const auto selected = Encoders::dataMatrix(QByteArray(capacities[i] * 2, '1'), false, 0, 0, true);
        QVERIFY(selected);
        QCOMPARE(selected->height, rowCounts[i]);
        QCOMPARE(selected->width, columnCounts[i]);
    }
    const auto columnsOnly = Encoders::dataMatrix(QByteArray(40, '1'), false, 0, 36, true);
    QVERIFY(columnsOnly);
    QCOMPARE(columnsOnly->height, 12);
    const auto rowsOnly = Encoders::dataMatrix(QByteArray(14, '1'), false, 8, 0, true);
    QVERIFY(rowsOnly);
    QCOMPARE(rowsOnly->width, 32);
    QVERIFY(!Encoders::dataMatrix("123456", false, 9, 18, true));
    QVERIFY(!Encoders::dataMatrix("123456", false, -8, 18, true));
    QVERIFY(!Encoders::dataMatrix(QByteArray(100, '1'), false, 0, 0, true));
    QVERIFY(!Encoders::dataMatrix(QByteArray(3117, '1'), false));
    const auto defaultSquare = Encoders::dataMatrix("123456", false);
    QVERIFY(defaultSquare);
    QCOMPARE(defaultSquare->height, 10);
    QCOMPARE(defaultSquare->width, 10);
    QCOMPARE(Encoders::dataMatrix("123456", false, 24)->modules,
             Encoders::dataMatrix("123456", false, 24, 24, false)->modules);
}

QTEST_MAIN(DataMatrixRectangularTests)
#include "test_datamatrix_rectangular.moc"
