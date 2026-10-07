#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSet>
#include <QtCore/QtEndian>
#include <QtTest/QTest>

using namespace Qt::StringLiterals;

namespace {
const auto resourcePrefix = u":/qtzpl/tests/golden/font0-size-sweep/"_s;
constexpr int firstSize = 4;
constexpr int lastSize = 512;
constexpr int sizeCount = lastSize - firstSize + 1;
constexpr int characterCount = 371;

QString digest(const QByteArray& data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}
}

// Integrity only: these tests do not render a font or assert pixel parity.
class Font0SweepReferencesTest final : public QObject {
    Q_OBJECT

    QJsonObject index_;
    QSet<QString> characters_;

private slots:
    void initTestCase()
    {
        QFile file(resourcePrefix + u"index.json"_s);
        QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(file.errorString()));
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(file.readAll(), &error);
        QCOMPARE(error.error, QJsonParseError::NoError);
        QVERIFY(document.isObject());
        index_ = document.object();
        QCOMPARE(index_[u"schemaVersion"_s].toInt(), 1);
        QCOMPARE(index_[u"source"_s].toString(), u"Labelary API"_s);
        QCOMPARE(index_[u"quality"_s].toString(), u"Bitonal"_s);
        QCOMPARE(index_[u"role"_s].toString(), u"validation"_s);
        QCOMPARE(index_[u"font"_s].toString(), u"0"_s);
        QVERIFY(index_[u"complete"_s].toBool());
        QCOMPARE(index_[u"completeSizes"_s].toInt(), sizeCount);
        QCOMPARE(index_[u"squareSizeRange"_s].toArray(), QJsonArray({firstSize, lastSize}));
        const auto sizes = index_[u"sizes"_s].toObject();
        QCOMPARE(sizes.size(), sizeCount);
        for (int size = firstSize; size <= lastSize; ++size) {
            const auto entry = sizes[QString::number(size)].toObject();
            QVERIFY2(!entry.isEmpty(), qPrintable(QString::number(size)));
            QVERIFY(entry[u"complete"_s].toBool());
            QCOMPARE(entry[u"glyphs"_s].toInt(), characterCount);
            QVERIFY(entry[u"pages"_s].toInt() > 0);
            QCOMPARE(entry[u"manifest"_s].toString(),
                     u"h%1/manifest.json"_s.arg(size, 3, 10, u'0'));
        }
        const auto characterArray = index_[u"characters"_s].toArray();
        QCOMPARE(characterArray.size(), characterCount);
        for (const auto value : characterArray) {
            const auto text = value.toString();
            QVERIFY(text.startsWith(u"U+"));
            bool ok = false;
            const auto cp = QStringView(text).sliced(2).toUInt(&ok, 16);
            QVERIFY(ok && cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff));
            QCOMPARE(text, u"U+"_s + QString::number(cp, 16).rightJustified(4, u'0').toUpper());
            QVERIFY(!characters_.contains(text));
            characters_.insert(text);
        }
        QCOMPARE(characters_.size(), characterCount);
    }

    void embeddedReferences_data()
    {
        QTest::addColumn<int>("fontSize");
        for (int size = firstSize; size <= lastSize; ++size)
            QTest::newRow(qPrintable(u"h%1"_s.arg(size, 3, 10, u'0'))) << size;
    }

    void embeddedReferences()
    {
        QFETCH(int, fontSize);
        const auto entry = index_[u"sizes"_s].toObject()[QString::number(fontSize)].toObject();
        const auto directory = u"h%1/"_s.arg(fontSize, 3, 10, u'0');
        QFile manifestFile(resourcePrefix + entry[u"manifest"_s].toString());
        QVERIFY2(manifestFile.open(QIODevice::ReadOnly), qPrintable(manifestFile.errorString()));
        const auto manifestBytes = manifestFile.readAll();
        QCOMPARE(digest(manifestBytes), entry[u"manifestSha256"_s].toString());
        QJsonParseError error;
        const auto document = QJsonDocument::fromJson(manifestBytes, &error);
        QCOMPARE(error.error, QJsonParseError::NoError);
        QVERIFY(document.isObject());
        const auto manifest = document.object();
        QCOMPARE(manifest[u"schemaVersion"_s].toInt(), 2);
        QCOMPARE(manifest[u"source"_s].toString(), u"Labelary API"_s);
        QCOMPARE(manifest[u"quality"_s].toString(), u"Bitonal"_s);
        QCOMPARE(manifest[u"role"_s].toString(), u"validation"_s);
        QCOMPARE(manifest[u"fontHeight"_s].toInt(), fontSize);
        QCOMPARE(manifest[u"fontWidth"_s].toInt(), fontSize);
        QCOMPARE(manifest[u"characters"_s].toArray(), index_[u"characters"_s].toArray());
        const auto cases = manifest[u"cases"_s].toArray();
        QCOMPARE(cases.size(), entry[u"pages"_s].toInt());
        QSet<QString> foundCharacters;
        QSet<QString> foundPages;
        for (const auto value : cases) {
            const auto page = value.toObject();
            const auto name = page[u"name"_s].toString();
            QVERIFY(!name.isEmpty() && !name.contains(u'/') && !name.contains(u'\\'));
            QVERIFY(!foundPages.contains(name));
            foundPages.insert(name);
            QCOMPARE(page[u"role"_s].toString(), u"validation"_s);
            QCOMPARE(page[u"font"_s].toString(), u"0"_s);
            QCOMPARE(page[u"fontHeight"_s].toInt(), fontSize);
            QCOMPARE(page[u"fontWidth"_s].toInt(), fontSize);
            QCOMPARE(page[u"anchor"_s].toString(), u"FT"_s);
            QCOMPARE(page[u"orientation"_s].toString(), u"N"_s);
            const int width = page[u"width"_s].toInt();
            const int height = page[u"height"_s].toInt();
            QVERIFY(width > 0 && width <= 8000 && height > 0 && height <= 8000);

            QFile zplFile(resourcePrefix + directory + name + u".zpl"_s);
            QVERIFY2(zplFile.open(QIODevice::ReadOnly), qPrintable(zplFile.errorString()));
            const auto zpl = zplFile.readAll();
            QCOMPARE(digest(zpl), page[u"zplSha256"_s].toString());
            QVERIFY(zpl.startsWith("^XA") && zpl.trimmed().endsWith("^XZ"));
            QFile pngFile(resourcePrefix + directory + name + u"-labelary-bitonal.png"_s);
            QVERIFY2(pngFile.open(QIODevice::ReadOnly), qPrintable(pngFile.errorString()));
            const auto png = pngFile.readAll();
            QCOMPARE(digest(png), page[u"pngSha256"_s].toString());
            QVERIFY(png.size() >= 33);
            QCOMPARE(png.first(8), QByteArray::fromHex("89504e470d0a1a0a"));
            QCOMPARE(qFromBigEndian<quint32>(png.constData() + 8), quint32(13));
            QCOMPARE(png.sliced(12, 4), QByteArray("IHDR"));
            QCOMPARE(qFromBigEndian<quint32>(png.constData() + 16), quint32(width));
            QCOMPARE(qFromBigEndian<quint32>(png.constData() + 20), quint32(height));
            QCOMPARE(quint8(png[24]), quint8(1)); // Original one-bit grayscale response.
            QCOMPARE(quint8(png[25]), quint8(0));
            QCOMPARE(quint8(png[26]), quint8(0));
            QCOMPARE(quint8(png[27]), quint8(0));
            QCOMPARE(quint8(png[28]), quint8(0));

            const auto cells = page[u"cells"_s].toArray();
            QVERIFY(!cells.isEmpty());
            for (const auto cellValue : cells) {
                const auto cell = cellValue.toObject();
                const auto cp = cell[u"codepoint"_s].toString();
                QVERIFY2(characters_.contains(cp), qPrintable(cp));
                QVERIFY2(!foundCharacters.contains(cp), qPrintable(cp));
                foundCharacters.insert(cp);
            }
        }
        QCOMPARE(foundCharacters.size(), characterCount);
        QCOMPARE(foundCharacters, characters_);
    }
};

QTEST_GUILESS_MAIN(Font0SweepReferencesTest)
#include "test_font0_sweep_references.moc"
