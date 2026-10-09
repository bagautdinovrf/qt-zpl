#include "../src/font0_face.hpp"
#include <QtCore/QCryptographicHash>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtTest/QTest>
#include <limits>

using namespace Qt::StringLiterals;

namespace {
QByteArray resource(const QString& name) {
  QFile file(u":/font0-face/"_s+name);
  return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray{};
}
QJsonObject coverage() {
  return QJsonDocument::fromJson(resource(u"embedded-font-coverage.json"_s)).object();
}
}

class Font0FaceTest final : public QObject {
  Q_OBJECT
private slots:
  void nativeCmapMatchesGeneratedAdvanceIndices() {
    const auto bytes=resource(u"font0.ttf"_s);
    const auto expected=coverage();
    QVERIFY(!bytes.isEmpty()); QVERIFY(!expected.isEmpty());
    QCOMPARE(QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex()),
      expected[u"fontSha256"_s].toString());
    QtZpl::Font0Face face(bytes); QVERIFY2(face.isValid(),qPrintable(face.errorString()));
    QCOMPARE(face.capHeight(),qreal(750));
    const auto mappings=expected[u"glyphIds"_s].toObject();
    QCOMPARE(mappings.size(),304);
    for(auto it=mappings.begin();it!=mappings.end();++it) {
      const char32_t cp=it.key().mid(2).toUInt(nullptr,16);
      const auto ids=face.glyphIndexesForString(QString::fromUcs4(&cp,1));
      QCOMPARE(ids.size(),1); QCOMPARE(ids.front(),quint32(it.value().toInt()));
    }
    const auto absent=expected[u"absentCodepoints"_s].toArray();
    QCOMPARE(absent.size(),13);
    for(const auto value:absent) {
      const char32_t cp=value.toString().mid(2).toUInt(nullptr,16);
      const auto ids=face.glyphIndexesForString(QString::fromUcs4(&cp,1));
      QCOMPARE(ids.size(),1); QCOMPARE(ids.front(),quint32(0));
    }
  }

  void invalidDataAndIndicesAreObservable() {
    for(const QByteArray bytes:{QByteArray{},QByteArray("not a font")}) {
      QtZpl::Font0Face face(bytes); QVERIFY(!face.isValid()); QVERIFY(!face.errorString().isEmpty());
      QVERIFY(!face.advance(0)); QVERIFY(!face.glyph(0));
    }
    QtZpl::Font0Face face(resource(u"font0.ttf"_s)); QVERIFY(face.isValid());
    const quint32 count=quint32(coverage()[u"glyphCount"_s].toInt()); QCOMPARE(count,quint32(340));
    for(const quint32 index:{count,std::numeric_limits<quint32>::max()}) {
      const auto advance=face.advance(index); const auto glyph=face.glyph(index);
      QVERIFY(!advance); QVERIFY(!advance.error().isEmpty());
      QVERIFY(!glyph); QVERIFY(!glyph.error().isEmpty());
    }
    // An invalid request does not poison this render's valid face or cache.
    QVERIFY(face.advance(0)); QVERIFY(face.glyph(0));
  }

  void unscaledAdvancesAgreeBeforeAndAfterOutlineLoading() {
    QtZpl::Font0Face face(resource(u"font0.ttf"_s)); QVERIFY(face.isValid());
    // These are the embedded .notdef and .null hmtx values, in the font's
    // 1000-unit grid. This detects accidental 16.16 conversion of NO_SCALE.
    const auto missingAdvance=face.advance(0); QVERIFY(missingAdvance); QCOMPARE(*missingAdvance,qreal(296));
    const auto nullAdvance=face.advance(1); QVERIFY(nullAdvance); QCOMPARE(*nullAdvance,qreal(0));
    const quint32 count=quint32(coverage()[u"glyphCount"_s].toInt());
    for(quint32 index=0;index<count;++index) {
      const auto before=face.advance(index); QVERIFY(before);
      const auto glyph=face.glyph(index); QVERIFY(glyph);
      const auto after=face.advance(index); QVERIFY(after);
      QCOMPARE(*before,glyph->advance); QCOMPARE(*after,*before);
    }
    QVERIFY(!face.glyph(count));
  }

  void supplementaryCharactersConsumeOneGlyph() {
    QtZpl::Font0Face face(resource(u"font0.ttf"_s)); QVERIFY(face.isValid());
    const auto ids=face.glyphIndexesForString(u"A\U0001F600Б");
    QCOMPARE(ids.size(),3); QCOMPARE(ids[0],quint32(36)); QCOMPARE(ids[1],quint32(0));
    QCOMPARE(ids[2],quint32(coverage()[u"glyphIds"_s].toObject()[u"U+0411"_s].toInt()));
    QString malformed; malformed+=QChar(0xD800); malformed+=u'A'; malformed+=QChar(0xDC00);
    const auto malformedIds=face.glyphIndexesForString(malformed);
    QCOMPARE(malformedIds,QList<quint32>({0,36,0}));
  }

  void returnedPathsDoNotMutateTheCacheAndOutliveTheFace() {
    QPainterPath retained;
    {
      QtZpl::Font0Face face(resource(u"font0.ttf"_s)); QVERIFY(face.isValid());
      const auto ids=face.glyphIndexesForString(u"H"); QCOMPARE(ids.size(),1);
      const auto original=face.glyph(ids.front()); QVERIFY(original); QVERIFY(!original->path.isEmpty());
      retained=original->path;
      auto changed=face.glyph(ids.front()); QVERIFY(changed); changed->path.translate(1000,1000);
      const auto cached=face.glyph(ids.front()); QVERIFY(cached); QCOMPARE(cached->path,retained);
      QVERIFY(changed->path!=retained);
    }
    QVERIFY(!retained.isEmpty()); QVERIFY(retained.boundingRect().isValid());
  }
};

// Native design extraction needs no platform font engine or QGuiApplication.
QTEST_GUILESS_MAIN(Font0FaceTest)
#include "test_font0_face.moc"
