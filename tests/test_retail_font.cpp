#include <QtTest/QTest>
#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtZpl/qtzpl.hpp>

using namespace Qt::StringLiterals;

class RetailFontTest final : public QObject {
    Q_OBJECT
private slots:
    void labelaryGoldens_data() {
        QTest::addColumn<QString>("name");
        const QDir directory(u":/qtzpl/tests/golden/retail-native-fonts"_s);
        for(const auto& file:directory.entryList({u"*.zpl"_s},QDir::Files))
            QTest::newRow(qPrintable(file)) << file.chopped(4);
    }
    void labelaryGoldens() {
        QFETCH(QString,name);
        const QString prefix=u":/qtzpl/tests/golden/retail-native-fonts/"_s+name;
        QFile file(prefix+u".zpl"_s);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QImage expected(prefix+u"-labelary-bitonal.png"_s);
        QVERIFY(!expected.isNull());
        const auto result=QtZpl::render(QString::fromUtf8(file.readAll()),{},
            QtZpl::RenderOptions{.width=expected.width(),.height=expected.height()});
        QVERIFY(result);
        QVERIFY2(result->diagnostics.isEmpty(),result->diagnostics.isEmpty()?"":qPrintable(result->diagnostics.front().message));
        QCOMPARE(result->labels.size(),1);
        const auto& actual=result->labels.front();
        QCOMPARE(actual.size(),expected.size());
        qsizetype differences=0;
        for(int y=0;y<expected.height();++y)
            for(int x=0;x<expected.width();++x)
                differences+=(qGray(actual.pixel(x,y))<128)!=(qGray(expected.pixel(x,y))<128);
        if(differences)actual.save(QCoreApplication::applicationDirPath()+u"/retail-"_s+name+u"-actual.png"_s);
        QCOMPARE(differences,0);
    }
};

QTEST_MAIN(RetailFontTest)
#include "test_retail_font.moc"
