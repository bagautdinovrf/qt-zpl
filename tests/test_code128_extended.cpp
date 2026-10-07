#include "../src/barcode_encoders.hpp"
#include <QtZpl/qtzpl.hpp>
#include <QtCore/QFile>
#include <QtGui/QImage>
#include <QtTest/QTest>
#include <algorithm>

using QtZpl::BarcodeEncoders::Detail::code128Codewords;

class Code128ExtendedTest final : public QObject {
    Q_OBJECT
private slots:
    void uccCaseCodewords() {
        const QVector<int> expected{105,102,0,12,34,56,78,90,12,34,56,75,42};
        const auto words=code128Codewords(u"0012345678901234567",u'U');
        QVERIFY(words.has_value());QCOMPARE(*words,expected);
        const auto redundantFlag=code128Codewords(u"0012345678901234567",u'U',true);
        QVERIFY(redundantFlag.has_value());QCOMPARE(*redundantFlag,expected);
    }
    void optionalMod10() {
        const auto words=code128Codewords(u">;>80012345678901234567",u'N',true);
        QVERIFY(words.has_value());
        QCOMPARE(*words,QVector<int>({105,102,0,12,34,56,78,90,12,34,56,75,42}));
        const auto automatic=code128Codewords(u"0012345678901234567",u'A',true);
        const auto explicitChecked=code128Codewords(u"00123456789012345675",u'A');
        QVERIFY(automatic.has_value());QCOMPARE(*automatic,*explicitChecked);
        const auto normal=code128Codewords(u"0012345678901234567",u'N',true);
        const auto normalChecked=code128Codewords(u"00123456789012345675",u'N');
        QVERIFY(normal.has_value());QCOMPARE(*normal,*normalChecked);
    }
    void rejectsInvalidInput() {
        QVERIFY(!code128Codewords(u"123",u'U'));
        QVERIFY(!code128Codewords(u"00123456789012345675",u'U'));
        QVERIFY(!code128Codewords(u"001234567890123456A",u'U'));
        QVERIFY(!code128Codewords(u"ABC123",u'N',true));
        QVERIFY(!code128Codewords(u">;>8ABC",u'N',true));
        QVERIFY(!code128Codewords(u">;>8",u'N',true));
        QVERIFY(!code128Codewords(QString(16385,u'9'),u'N',true));
    }
    void longSubsetBChecksumDoesNotOverflow() {
        const QString input(8192,u'~');
        const auto words=code128Codewords(input,u'B');QVERIFY(words);
        qint64 checksum=words->front();
        for(qsizetype i=1;i+1<words->size();++i)checksum+=qint64(i)*(*words)[i];
        QCOMPARE(words->back(),int(checksum%103));
    }
    void excessiveRasterProducesDiagnostic() {
        const auto result=QtZpl::render(QStringLiteral("^XA^PW100^LL100^FO0,0^BCN,2147483647,N^FD123456^FS^XZ"));
        QVERIFY(result);
        QVERIFY(std::ranges::any_of(result->diagnostics,[](const auto& diagnostic){return diagnostic.code==QStringLiteral("barcode-dimensions");}));
    }
    void matchesLabelaryWorkingUccPath_data() {
        QTest::addColumn<QString>("zpl");
        QTest::newRow("case-u")<<QStringLiteral("^XA^PW812^LL203^BY2^FO20,60^BCN,60,N,N,N,U^FD0012345678901234567^FS^XZ");
        // Labelary e=Y is defective; use its U-mode oracle for equivalent data.
        QTest::newRow("explicit-check")<<QStringLiteral("^XA^PW812^LL203^BY2^FO20,60^BCN,60,N,N,Y,N^FD>;>80012345678901234567^FS^XZ");
    }
    void matchesLabelaryWorkingUccPath() {
        QFETCH(QString,zpl);
        const QImage expected(QStringLiteral(":/qtzpl/tests/golden/code128-extended/ucc-u-labelary-bitonal.png"));
        QVERIFY(!expected.isNull());
        const auto actual=QtZpl::render(zpl);
        QVERIFY(actual.has_value());QVERIFY(actual->diagnostics.isEmpty());
        QCOMPARE(actual->labels.front().convertToFormat(QImage::Format_RGB32),expected.convertToFormat(QImage::Format_RGB32));
    }
    void caseModeCaption_data() {
        QTest::addColumn<QString>("name");
        for(const char* name:{"ucc-u-caption","ucc-u-default-caption","ucc-u-above-caption","caption-modules","caption-origins-rotations"})
            QTest::newRow(name)<<QString::fromLatin1(name);
    }
    void caseModeCaption() {
        QFETCH(QString,name);
        const QString base=QStringLiteral(":/qtzpl/tests/golden/code128-extended/")+name;
        QFile input(base+QStringLiteral(".zpl"));QVERIFY(input.open(QIODevice::ReadOnly));
        const QImage expected(base+QStringLiteral("-labelary-bitonal.png"));QVERIFY(!expected.isNull());
        const auto actual=QtZpl::render(QString::fromLatin1(input.readAll()));
        QVERIFY(actual.has_value());QVERIFY(actual->diagnostics.isEmpty());
        QCOMPARE(actual->labels.front().convertToFormat(QImage::Format_RGB32),expected.convertToFormat(QImage::Format_RGB32));
    }
};

QTEST_MAIN(Code128ExtendedTest)
#include "test_code128_extended.moc"
