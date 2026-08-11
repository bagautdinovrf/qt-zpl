#include <QtZpl/qtzpl.hpp>
#include <QtCore/QCoreApplication>
#include <QtCore/QDebug>

using namespace Qt::StringLiterals;

int main(int argc,char** argv){
  QCoreApplication app(argc,argv);
  auto result=QtZpl::render(u"^XA^PW400^LL200^FO20,20^A0N,30,30^FDHello QtZpl^FS^FO20,70^GB300,80,3^FS^XZ");
  if(!result){qCritical()<<result.error().message;return 1;}
  return result->labels.front().save(u"label.png"_s)?0:2;
}
