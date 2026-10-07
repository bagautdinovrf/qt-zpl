#include <QtZpl/qtzpl.hpp>
#include <QtCore/QFile>
#include <QtGui/QImage>
#include <QtTest/QTest>

class BarcodeAnchorsTest final : public QObject {
    Q_OBJECT
private slots:
    void labelaryParity_data() {
        QTest::addColumn<QString>("name");
        for(const char* code:{"b3","be"})for(const char* anchor:{"fo","ft"})for(const char orientation:QByteArray("nrib")){
            const QString name=QStringLiteral("%1-%2-%3").arg(QString::fromLatin1(code),QString::fromLatin1(anchor),QChar::fromLatin1(orientation));
            QTest::newRow(qPrintable(name))<<name;
        }
    }
    void labelaryParity() {
        QFETCH(QString,name);
        const QString base=QStringLiteral(":/qtzpl/tests/golden/barcode-anchors/")+name;
        QFile input(base+QStringLiteral(".zpl"));QVERIFY(input.open(QIODevice::ReadOnly));
        const QImage expected(base+QStringLiteral("-labelary-bitonal.png"));QVERIFY(!expected.isNull());
        const auto actual=QtZpl::render(QString::fromLatin1(input.readAll()));
        QVERIFY(actual.has_value());QVERIFY(actual->diagnostics.isEmpty());
        QCOMPARE(actual->labels.size(),1);
        QCOMPARE(actual->labels.front().convertToFormat(QImage::Format_RGB32),expected.convertToFormat(QImage::Format_RGB32));
    }
};

QTEST_GUILESS_MAIN(BarcodeAnchorsTest)
#include "test_barcode_anchors.moc"
