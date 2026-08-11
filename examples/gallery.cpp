#include <QtZpl/qtzpl.hpp>

#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QTextStream>
#include <QtGui/QGuiApplication>

using namespace Qt::StringLiterals;

namespace {

struct Example {
  QString fileName;
  QString zpl;
  QtZpl::RenderOptions options;
};

bool saveExample(const Example& example,const QDir& output) {
  auto rendered=QtZpl::render(example.zpl,{},example.options);
  if(!rendered||rendered->labels.isEmpty()) {
    QTextStream(stderr)<<"Failed to render "<<example.fileName<<": "
      <<(rendered?u"no labels"_s:rendered.error().message)<<Qt::endl;
    return false;
  }
  for(const auto& diagnostic:rendered->diagnostics)
    QTextStream(stderr)<<example.fileName<<": "<<diagnostic.code<<": "<<diagnostic.message<<Qt::endl;
  const auto path=output.filePath(example.fileName);
  if(!rendered->labels.front().save(path,"PNG")) {
    QTextStream(stderr)<<"Failed to save "<<path<<Qt::endl;return false;
  }
  QTextStream(stdout)<<QFileInfo(path).absoluteFilePath()<<Qt::endl;
  return true;
}

} // namespace

int main(int argc,char** argv) {
  QGuiApplication app(argc,argv);
  const QString outputPath=argc>1?QString::fromLocal8Bit(argv[1]):u"examples/rendered"_s;
  QDir output;
  if(!output.mkpath(outputPath)||!output.cd(outputPath)) {
    QTextStream(stderr)<<"Cannot create output directory: "<<outputPath<<Qt::endl;return 1;
  }

  const QList<Example> examples{
    Example{
      u"01-basic.png"_s,
      u"^XA^PW600^LL360^CI28^CF0,34"
      "^FO30,25^GB540,310,4^FS"
      "^FO55,50^A0N,46,42^FDQtZpl renderer^FS"
      "^FO55,120^A0N,30,28^FDC++23 / Qt 6.11^FS"
      "^FO55,175^GB490,3,3^FS"
      "^FO55,215^A0N,28,26^FDZPL to QImage locally^FS^XZ"_s,
      {.dpi=203}
    },
    Example{
      u"02-codereprint-label.png"_s,
      u"^XA^CI28^CF0,30"
      "^FO20,35^FDАртикул: 267884^FS"
      "^FO20,80^FDПроизведено: 11.08.2026^FS"
      "^FO20,125^FDГоден до: 11.08.2028^FS"
      "^FO20,165^FDСерия: A260811^FS"
      "^FO20,215^GB750,3,3^FS"
      "^FO20,250^FDНоменклатура: Тестовый препарат^FS"
      "^FO20,295^FDПроизводитель: ООО Пример^FS"
      "^FT80,650^BXN,6,200,36,36,1,|"
      "^FD|10109501101530003|d02917280811|d02910A260811^FS"
      "^FO500,390^BY3^BEN,110,Y,N^FD5901234123457^FS"
      "^FO20,850^GB1080,3,3^FS"
      "^FO80,910^A0N,32,30^FDПРИМЕР ЭТИКЕТКИ CODEREPRINT^FS^XZ"_s,
      {.dpi=300,.width=1181,.height=1181}
    },
    Example{
      u"03-graphics-and-barcodes.png"_s,
      u"^XA^PW800^LL520^CI28^CF0,28"
      "^FO30,30^GB140,140,140^FS"
      "^FO65,65^FR^GB140,140,140^FS"
      "^FO95,95^GB50,50,50^FS"
      "^FX 8x8 embedded bitmap^FO220,40^GFA,8,8,1,3C42818181423C00^FS"
      "^FO220,100^FDReverse graphics and embedded bitmap^FS"
      "^FO30,230^BXN,5,200,36,36,1,|^FD|10109501101530003|d02910DEMO42^FS"
      "^FO330,260^BY3^BEN,100,Y,N^FD4601234567893^FS^XZ"_s,
      {.dpi=203}
    }
  };

  bool success=true;
  for(const auto& example:examples)success=saveExample(example,output)&&success;
  return success?0:2;
}
