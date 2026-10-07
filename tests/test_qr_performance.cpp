#include <QtTest/QTest>
#include "../src/barcode_encoders.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {
using namespace Qt::StringLiterals;
namespace Encoders = QtZpl::BarcodeEncoders;

// Frozen pre-optimization Zebra penalty. The reference deliberately keeps the
// old eight-independent-encodes strategy so it can detect changes to candidate
// remasking, format bits, ECC boosting, or tie ordering in the optimized path.
int legacyZebraQrPenalty(const Encoders::Matrix& matrix) {
  int penalty=0;
  const auto linePenalty=[&](bool vertical){
    int result=0;
    for(int fixed=0;fixed<matrix.width;++fixed){
      bool previous=vertical?matrix.at(fixed,0):matrix.at(0,fixed);int run=1;
      for(int moving=1;moving<matrix.width;++moving){
        const bool current=vertical?matrix.at(fixed,moving):matrix.at(moving,fixed);
        if(current==previous)++run;
        else{if(run>=5)result+=3+run-5;previous=current;run=1;}
      }
      if(run>=5)result+=3+run-5;
    }
    return result;
  };
  penalty+=linePenalty(false)+linePenalty(true);
  for(int y=0;y<matrix.height-1;++y)for(int x=0;x<matrix.width-1;++x){
    const bool value=matrix.at(x,y);
    if(matrix.at(x+1,y)==value&&matrix.at(x,y+1)==value&&matrix.at(x+1,y+1)==value)penalty+=3;
  }
  const auto finderPenalty=[&](bool vertical){
    int result=0;
    for(int fixed=0;fixed<matrix.width;++fixed)for(int start=0;start<=matrix.width-7;++start){
      const auto at=[&](int moving){return vertical?matrix.at(fixed,moving):matrix.at(moving,fixed);};
      if(at(start)&&!at(start+1)&&at(start+2)&&at(start+3)&&at(start+4)&&!at(start+5)&&at(start+6)){
        bool before=true;for(int i=std::max(0,start-4);before&&i<start;++i)before=!at(i);
        bool after=true;for(int i=start+7;after&&i<std::min(matrix.width,start+11);++i)after=!at(i);
        if(before||after)result+=40;
      }
    }
    return result;
  };
  penalty+=finderPenalty(false)+finderPenalty(true);
  int dark=0;for(bool module:matrix.modules)dark+=module;
  penalty+=(std::abs(dark*2-matrix.width*matrix.height)*10/(matrix.width*matrix.height))*10;
  return penalty;
}
}

class QrPerformanceTest final : public QObject {
  Q_OBJECT

private slots:
  void automaticMaskMatchesLegacySelection_data() {
    QTest::addColumn<QByteArray>("payload");
    QTest::addColumn<QChar>("ecc");
    const std::pair<QByteArray, QByteArray> modes[] = {
      {"numeric", "0123456789"},
      {"alphanumeric", "AB12 $%*+-./:"},
      {"byte", "qtzpl123!?/"}
    };
    for (const char ecc : {'L', 'M', 'Q', 'H'}) {
      for (const auto& [name, pattern] : modes) {
        for (const int size : {1, 24, 128, 1024}) {
          const QByteArray payload = pattern.repeated((size + pattern.size() - 1) / pattern.size()).first(size);
          const QByteArray row = name + '-' + QByteArray::number(size) + '-' + ecc;
          QTest::newRow(row.constData()) << payload << QChar::fromLatin1(ecc);
        }
      }
      QTest::newRow((QByteArray("empty-") + ecc).constData()) << QByteArray{} << QChar::fromLatin1(ecc);
    }
  }

  void automaticMaskMatchesLegacySelection() {
    QFETCH(QByteArray, payload);
    QFETCH(QChar, ecc);
    Encoders::Matrix expected;
    int minimumPenalty = std::numeric_limits<int>::max();
    for (int mask = 0; mask < 8; ++mask) {
      auto candidate = Encoders::qrCode(payload, ecc, mask);
      QVERIFY2(candidate.has_value(), candidate ? "" : qPrintable(candidate.error()));
      const int penalty = legacyZebraQrPenalty(*candidate);
      if (penalty < minimumPenalty) {
        minimumPenalty = penalty;
        expected = std::move(*candidate);
      }
    }
    const auto actual = Encoders::qrCode(payload, ecc);
    QVERIFY2(actual.has_value(), actual ? "" : qPrintable(actual.error()));
    QCOMPARE(actual->width, expected.width);
    QCOMPARE(actual->height, expected.height);
    QCOMPARE(actual->modules, expected.modules);
  }

  void errorAndDefaultBehaviorIsPreserved() {
    const QByteArray payload = "qtzpl123";
    for (const int mask : {8, 99}) {
      const auto invalid = Encoders::qrCode(payload, u'M', mask);
      QVERIFY(!invalid.has_value());
      QCOMPARE(invalid.error(), u"Invalid value"_s);
    }
    const QByteArray tooLong(4096, 'q');
    for (const QChar ecc : {QChar(u'L'), QChar(u'M'), QChar(u'Q'), QChar(u'H')}) {
      const auto automatic = Encoders::qrCode(tooLong, ecc);
      const auto fixed = Encoders::qrCode(tooLong, ecc, 0);
      QVERIFY(!automatic.has_value());
      QVERIFY(!fixed.has_value());
      QCOMPARE(automatic.error(), fixed.error());
    }
    const auto normal = Encoders::qrCode(payload, u'M');
    const auto negativeMask = Encoders::qrCode(payload, u'M', -2);
    const auto defaultEcc = Encoders::qrCode(payload, u'?');
    const auto lowerCaseEcc = Encoders::qrCode(payload, u'm');
    QVERIFY(normal.has_value());
    QVERIFY(negativeMask.has_value());
    QVERIFY(defaultEcc.has_value());
    QVERIFY(lowerCaseEcc.has_value());
    QCOMPARE(negativeMask->modules, normal->modules);
    QCOMPARE(defaultEcc->modules, normal->modules);
    QCOMPARE(lowerCaseEcc->modules, normal->modules);
  }
};

QTEST_APPLESS_MAIN(QrPerformanceTest)
#include "test_qr_performance.moc"
