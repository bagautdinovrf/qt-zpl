#include "../src/maxicode_encoder.hpp"
#include <QtZpl/qtzpl.hpp>
#include <QtCore/QFile>
#include <QtGui/QImage>
#include <QtTest/QTest>

using QtZpl::BarcodeEncoders::MaxiCode::encode;

class MaxiCodeExtendedTest final : public QObject {
    Q_OBJECT
private slots:
    void authoritativeCodewords() {
        // Zint 2.15.0 backend/tests/test_maxicode.c test_input row 0.
        // These include the whole primary RS block, not just its mode byte.
        const auto symbol=encode("A",4);
        QVERIFY(symbol.has_value());
        const QByteArray expected=QByteArray::fromHex("04012121212121212121080e192b200c2406321c2121212121212121");
        for(qsizetype i=0;i<expected.size();++i)QCOMPARE(symbol->codewords[i],int(static_cast<unsigned char>(expected[i])));
        // Upstream row 59: structured append part 1/2 is two extra words.
        const auto appended=encode("A",4,1,2);
        QVERIFY(appended.has_value());
        const QByteArray appendExpected=QByteArray::fromHex("04210101212121212121090b2603370e2527071e2121212121212121");
        for(qsizetype i=0;i<appendExpected.size();++i)QCOMPARE(appended->codewords[i],int(static_cast<unsigned char>(appendExpected[i])));
    }
    void mode3PrimaryVector() {
        // Zint test_input row 17, postcode ABCDEF / country123 / service456.
        const QByteArray data=QByteArrayLiteral("456123ABCDEF[)>\x1e" "01\x1d" "96A\x1e\x04");
        const auto symbol=encode(data,3);
        QVERIFY(symbol.has_value());
        const QByteArray expected=QByteArray::fromHex("231101312010301e201c3c1d220319150f200f2a");
        for(qsizetype i=0;i<expected.size();++i)QCOMPARE(symbol->codewords[i],int(static_cast<unsigned char>(expected[i])));
    }
    void modesAndCapacity() {
        for(int mode=4;mode<=6;++mode){
            const auto symbol=encode("HELLO WORLD 123456789",mode);
            QVERIFY(symbol.has_value());
            QCOMPARE(symbol->codewords[0],mode);
            QCOMPARE(symbol->codewords.size(),144);
            QCOMPARE(symbol->grid.width,30);QCOMPARE(symbol->grid.height,33);
            QVERIFY(encode(QByteArray(mode==5?77:93,'A'),mode));
            QVERIFY(!encode(QByteArray(mode==5?78:94,'A'),mode));
            QVERIFY(!encode(QByteArray(mode==5?77:93,'A'),mode,1,2));
        }
    }
    void validation() {
        QVERIFY(!encode("A",1));QVERIFY(!encode("A",7));
        QVERIFY(!encode({},4));QVERIFY(!encode("A",4,0,1));
        QVERIFY(!encode("A",4,2,1));QVERIFY(!encode("A",4,1,9));
        QVERIFY(!encode("A",3));
        QVERIFY(!encode(QByteArrayLiteral("456123abcDEF[)>\x1e" "01\x1d" "96A\x1e\x04"),3));
        QVERIFY(!encode(QByteArrayLiteral("45+123ABCDEF[)>\x1e" "01\x1d" "96A\x1e\x04"),3));
    }
    void labelaryParity_data() {
        QTest::addColumn<QString>("name");
        for(const char* name:{"mode3","mode4","mode5","mode6","append","ft-mode4"})
            QTest::newRow(name)<<QString::fromLatin1(name);
    }
    void labelaryParity() {
        QFETCH(QString,name);
        const QString base=QStringLiteral(":/qtzpl/tests/golden/maxicode-extended/")+name;
        QFile input(base+QStringLiteral(".zpl"));QVERIFY(input.open(QIODevice::ReadOnly));
        const QImage expected(base+QStringLiteral("-labelary-bitonal.png"));QVERIFY(!expected.isNull());
        const auto actual=QtZpl::render(QString::fromLatin1(input.readAll()));
        QVERIFY(actual.has_value());QVERIFY(actual->diagnostics.isEmpty());
        QCOMPARE(actual->labels.size(),1);
        QCOMPARE(actual->labels.front().size(),expected.size());
        QCOMPARE(actual->labels.front().convertToFormat(QImage::Format_RGB32),expected.convertToFormat(QImage::Format_RGB32));
    }
};

QTEST_GUILESS_MAIN(MaxiCodeExtendedTest)
#include "test_maxicode_extended.moc"
