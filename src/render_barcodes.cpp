// Barcode parameter resolution and symbol drawing.
#include "render_internal.hpp"
#include "barcode_encoders.hpp"
#include "pdf417_encoder.hpp"
#include "maxicode_encoder.hpp"
#include "linear_encoders.hpp"
#include "aztec_encoder.hpp"
#include "databar_encoder.hpp"
#include "micropdf417_encoder.hpp"
#include "printer_font_metrics.hpp"
#include "printer_font_raster.hpp"
#include "retail_font.hpp"
#include "graphic_geometry.hpp"
#include "font0_advance_data.hpp"
#include "font0_face.hpp"
#include <QtCore/QByteArray>
#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QtMath>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QFontDatabase>
#include <QtGui/QTransform>
#include <algorithm>
#include <bit>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <optional>
#include <utility>

namespace QtZpl::Rendering {
using namespace Qt::StringLiterals;

namespace {
void placeNewSymbol(QImage& image,const QImage& original,const State& state,Orientation orientation);
} // namespace

namespace {

QString code39Pattern(QChar c) {
  static const QHash<QChar,QString> patterns{
    {u'0',u"nnnwwnwnn"_s},{u'1',u"wnnwnnnnw"_s},{u'2',u"nnwwnnnnw"_s},{u'3',u"wnwwnnnnn"_s},
    {u'4',u"nnnwwnnnw"_s},{u'5',u"wnnwwnnnn"_s},{u'6',u"nnwwwnnnn"_s},{u'7',u"nnnwnnwnw"_s},
    {u'8',u"wnnwnnwnn"_s},{u'9',u"nnwwnnwnn"_s},{u'A',u"wnnnnwnnw"_s},{u'B',u"nnwnnwnnw"_s},
    {u'C',u"wnwnnwnnn"_s},{u'D',u"nnnnwwnnw"_s},{u'E',u"wnnnwwnnn"_s},{u'F',u"nnwnwwnnn"_s},
    {u'G',u"nnnnnwwnw"_s},{u'H',u"wnnnnwwnn"_s},{u'I',u"nnwnnwwnn"_s},{u'J',u"nnnnwwwnn"_s},
    {u'K',u"wnnnnnnww"_s},{u'L',u"nnwnnnnww"_s},{u'M',u"wnwnnnnwn"_s},{u'N',u"nnnnwnnww"_s},
    {u'O',u"wnnnwnnwn"_s},{u'P',u"nnwnwnnwn"_s},{u'Q',u"nnnnnnwww"_s},{u'R',u"wnnnnnwwn"_s},
    {u'S',u"nnwnnnwwn"_s},{u'T',u"nnnnwnwwn"_s},{u'U',u"wwnnnnnnw"_s},{u'V',u"nwwnnnnnw"_s},
    {u'W',u"wwwnnnnnn"_s},{u'X',u"nwnnwnnnw"_s},{u'Y',u"wwnnwnnnn"_s},{u'Z',u"nwwnwnnnn"_s},
    {u'-',u"nwnnnnwnw"_s},{u'.',u"wwnnnnwnn"_s},{u' ',u"nwwnnnwnn"_s},{u'*',u"nwnnwnwnn"_s}
  };
  return patterns.value(c.toUpper());
}

bool barcodeRasterAllowed(qint64 width,qint64 height,const Barcode& barcode,QList<Diagnostic>& diagnostics) {
  if(width>0&&height>0&&width<=32000&&height<=32000&&width*height<=64*1024*1024)return true;
  diagnostics.append({Severity::Error,u"barcode-size"_s,u"Barcode raster dimensions exceed the supported allocation limit."_s,-1,u'^'+barcode.symbology});
  return false;
}

void drawCode39(QImage& image, State& state, const Barcode& barcode, const ResolvedBarcode& code, const QString& data, const RenderOptions& options,QList<Diagnostic>& diagnostics) {
  const int narrow=code.moduleWidth;
  const int wide=std::max(narrow+1,qRound(narrow*code.wideToNarrowRatio));
  const int height=code.height;
  const QString encoded=u'*'+data.toUpper()+u'*';
  qint64 width=-qint64(narrow);
  for(const auto c:encoded)for(const auto element:code39Pattern(c))width+=element==u'w'?wide:narrow;
  width+=qint64(encoded.size())*narrow;
  if(width<=0)return;
  if(!barcodeRasterAllowed(width,height,barcode,diagnostics))return;
  QImage symbol(int(width),height,QImage::Format_ARGB32_Premultiplied);symbol.fill(options.background);
  int x=0;
  QPainter painter(&symbol); painter.setBrush(options.foreground); painter.setPen(Qt::NoPen);
  for(const auto c:encoded){
    const auto pattern=code39Pattern(c); if(pattern.isEmpty()) continue;
    for(int i=0;i<pattern.size();++i){const int w=pattern[i]==u'w'?wide:narrow;if(i%2==0)painter.drawRect(x,0,w,height);x+=w;}x+=narrow;
  }
  painter.end();
  QPoint position=state.position;
  if(state.baseline)switch(code.orientation){
    case Orientation::Normal:position.ry()-=height;break;
    case Orientation::Rotated90:break;
    case Orientation::Inverted:position.rx()-=int(width);break;
    case Orientation::BottomUp:position-=QPoint(height,int(width));break;
  }
  symbol=rotateImage(symbol,code.orientation);
  observeRaster(state,position,symbol);
  QPainter target(&image);target.drawImage(position,symbol);
}

QByteArray decodeDataMatrixData(const QByteArray& input, char marker, bool gs1) {
  QByteArray output;
  for(qsizetype i=0;i<input.size();++i) {
    if(input[i]!=marker||i+1>=input.size()){output.append(input[i]);continue;}
    const char next=input[i+1];
    if(next==marker){output.append(marker);++i;}
    else if(next=='1'){if(!(gs1&&output.isEmpty()))output.append(char(0x1d));++i;}
    else if((next=='d'||next=='D')&&i+4<input.size()&&input[i+2]>='0'&&input[i+2]<='9'&&input[i+3]>='0'&&input[i+3]<='9'&&input[i+4]>='0'&&input[i+4]<='9'){
      output.append(char((input[i+2]-'0')*100+(input[i+3]-'0')*10+input[i+4]-'0'));i+=4;
    } else output.append(input[i]);
  }
  return output;
}

void drawDataMatrix(QImage& image,State& state,const ResolvedBarcode& code,const QByteArray& text,const RenderOptions& options,QList<Diagnostic>& diagnostics) {
  const auto& matrixOptions=std::get<DataMatrixOptions>(code.options);
  const int module=code.moduleWidth;
  const bool gs1=matrixOptions.gs1;
  if(text.size()>16384){diagnostics.append({Severity::Error,u"datamatrix-encode"_s,u"DataMatrix field exceeds the maximum capacity, including control escapes."_s,-1,u"^BX"_s});return;}
  const auto decoded=decodeDataMatrixData(text,static_cast<char>(matrixOptions.escape.unicode()),gs1);
  const auto lowercase=gs1&&!decoded.contains(char(0x1d))?text.toLower():QByteArray{};
  if(lowercase.contains("\\u001d")||lowercase.contains("\\x1d"))
    diagnostics.append({Severity::Warning,u"gs1-datamatrix-separator"_s,
      u"GS1 DataMatrix field data contains a literal \\\\u001D or \\\\x1D text sequence; use byte 0x1D, |d029, or ^FH hex instead of an escape spelling."_s,-1,u"^BX"_s});
  auto matrix=BarcodeEncoders::dataMatrix(decoded,gs1,matrixOptions.rows,matrixOptions.columns,matrixOptions.rectangular);
  if(!matrix){diagnostics.append({Severity::Error,u"datamatrix-encode"_s,matrix.error(),-1,u"^BX"_s});return;}
  // Validate in 64 bits before multiplying dimensions for QImage.
  if(qint64(matrix->width)*matrix->height*module*module>64*1024*1024){
    diagnostics.append({Severity::Error,u"barcode-size"_s,u"Invalid or unsafe DataMatrix module size."_s,-1,u"^BX"_s});return;
  }
  QImage symbol(matrix->width*module,matrix->height*module,QImage::Format_ARGB32_Premultiplied);symbol.fill(options.background);
  QPainter p(&symbol);p.setPen(Qt::NoPen);p.setBrush(options.foreground);
  for(int y=0;y<matrix->height;++y)for(int x=0;x<matrix->width;++x)if(matrix->at(x,y))p.drawRect(x*module,y*module,module,module);
  p.end();placeNewSymbol(image,symbol,state,code.orientation);
}

} // namespace

int qrMagnification(const Barcode& barcode) {
  bool valid=false;
  const int requested=barcode.parameters.size()>2?barcode.parameters[2].toInt(&valid):0;
  return valid&&requested>0?std::min(requested,100):2;
}

namespace {

void drawQrCode(QImage& image,State& state,const Barcode& barcode,const ResolvedBarcode& code,const QByteArray& fieldData,const RenderOptions& options,QList<Diagnostic>& diagnostics) {
  QChar errorCorrection=u'M';
  QByteArray data=fieldData;
  if(data.size()>=3&&data[2]==','){
    errorCorrection=QChar::fromLatin1(data[0]).toUpper();
    const QChar inputMode=QChar::fromLatin1(data[1]).toUpper();
    data.remove(0,3);
    if(inputMode==u'M'&&!data.isEmpty()){
      const QChar characterMode=QChar::fromLatin1(data.front()).toUpper();
      if(characterMode==u'N'||characterMode==u'A'||characterMode==u'K'){
        data.remove(0,1);
        // Zebra manual alphanumeric mode uses the QR alphanumeric alphabet;
        // lower-case input is normalized to upper case by the printer.
        if(characterMode==u'A'){
          data=data.toUpper();
          data.removeIf([](char value){
            return !QStringView{u"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:"}.contains(QChar::fromLatin1(value));
          });
        }
      }
      else if(characterMode==u'B'&&data.size()>=5){
        bool ok=false;const int bytes=data.mid(1,4).toInt(&ok);
        if(ok&&bytes<=data.size()-5)data=data.mid(5,bytes);
      }
    }
  }
  const int module=code.moduleWidth;
  auto matrix=BarcodeEncoders::qrCode(data,errorCorrection);
  if(!matrix){diagnostics.append({Severity::Error,u"qrcode-encode"_s,matrix.error(),-1,u"^BQ"_s});return;}
  if(!barcodeRasterAllowed(qint64(matrix->width)*module,qint64(matrix->height)*module,barcode,diagnostics))return;
  QImage symbol(matrix->width*module,matrix->height*module,QImage::Format_ARGB32_Premultiplied);symbol.fill(options.background);
  QPainter painter(&symbol);painter.setPen(Qt::NoPen);painter.setBrush(options.foreground);
  for(int y=0;y<matrix->height;++y)for(int x=0;x<matrix->width;++x)if(matrix->at(x,y))painter.drawRect(x*module,y*module,module,module);
  painter.end();
  State placed=state;
  if(state.baseline)placed.position.ry()-=3*module;
  else{
    // ^FO keeps a ^BY-height margin before the symbol. The effective field
    // orientation places it (top for N, left for B, none for R and I), but
    // ^FW and the orientation slot never rotate the QR modules themselves.
    // The typeset anchor instead includes the lower quiet-zone margin.
    const int margin=std::max(0,state.barcodeDefaults.height);
    if(code.orientation==Orientation::Normal)placed.position.ry()+=margin;
    else if(code.orientation==Orientation::BottomUp)placed.position.rx()+=margin;
  }
  placeNewSymbol(image,symbol,placed,Orientation::Normal);
}

QString code128Interpretation(QStringView data) {
  QString result;result.reserve(data.size());
  for(qsizetype i=0;i<data.size();++i){
    if(data[i]==u'>'&&i+1<data.size()&&QStringView{u"<0=123456789:;"}.contains(data[i+1])){
      if(data[i+1]==u'<')result.append(u'>');
      ++i;continue;
    }
    result.append(data[i]);
  }
  return result;
}

void drawCode128(QImage& image,State& state,const ResolvedBarcode& code,const QString& text,const RenderOptions& options,QList<Diagnostic>& diagnostics,RenderFonts& fonts) {
  if(text.size()>4096){diagnostics.append({Severity::Error,u"code128-encode"_s,u"Code 128 rendered field data exceeds 4096 characters."_s,state.barcodeOffset,state.barcodeSource});return;}
  const auto rasterAllowed=[&](qint64 width,qint64 height){
    if(width>0&&height>0&&width<=32000&&height<=32000&&width*height<=64*1024*1024)return true;
    diagnostics.append({Severity::Error,u"barcode-dimensions"_s,u"Code 128 raster exceeds the supported dimensions or 64 megapixels."_s,state.barcodeOffset,state.barcodeSource});return false;
  };
  const auto& code128=std::get<Code128Options>(code.options);
  const QChar mode=code128.mode;
  const bool uccCheck=code128.uccCheckDigit;
  auto modules=BarcodeEncoders::code128(text,mode,uccCheck);
  if(!modules){diagnostics.append({Severity::Error,u"code128-encode"_s,modules.error(),-1,u"^BC"_s});return;}
  const int module=code.moduleWidth;
  const int height=code.height;
  if(!rasterAllowed(qint64(modules->size())*module,height))return;
  const bool interpretation=code.interpretation;
  const bool interpretationAbove=code.interpretationAbove;
  if(interpretation&&!state.barcodeInterpretationFont){
    const auto captionData=BarcodeEncoders::Detail::code128UccData(text,mode,uccCheck);
    const QString captionText=code128Interpretation(captionData?*captionData:text);
    const qreal fontSize=module*9.0;
    const qreal advance=fontSize*1365.0/2048;
    const int barsWidth=int(modules->size())*module;
    const int baseline=interpretationAbove?-(2*module+8):height+7*module+6;
    const int turns=code.orientation==Orientation::Rotated90?1:code.orientation==Orientation::Inverted?2:code.orientation==Orientation::BottomUp?3:0;
    qreal cursor=(barsWidth-advance*captionText.size())/2;
    struct PositionedGlyph {PrinterGlyph glyph;QPoint baseline;};
    QList<PositionedGlyph> glyphs;
    QRect bounds(0,0,barsWidth,height);
    for(const QChar character:captionText){
      const auto glyph=fonts.fontARaster().glyph(PrinterFontMetrics::fontACharacter(character.unicode()),int(fontSize),int(fontSize),turns);
      if(!glyph){diagnostics.append({Severity::Error,u"font-raster"_s,glyph.error(),state.barcodeOffset,state.barcodeSource});return;}
      // Round the physical position after the field rotation. A centered
      // caption can land on half-dots; reflecting a rounded normal position
      // selects the opposite tie at 180 and 270 degrees.
      const int glyphX=turns>=2?barsWidth-qRound(barsWidth-cursor):qRound(cursor);
      const QPoint position(glyphX,baseline);
      bounds|=QRect(position+glyph->bearing,glyph->alpha.size());
      glyphs.append({*glyph,position});
      cursor+=advance;
    }
    if(!rasterAllowed(bounds.width(),bounds.height()))return;
    QImage symbol(bounds.size(),QImage::Format_ARGB32_Premultiplied);symbol.fill(options.background);
    QPainter painter(&symbol);painter.setRenderHint(QPainter::Antialiasing,false);painter.setPen(Qt::NoPen);painter.setBrush(options.foreground);
    for(qsizetype i=0;i<modules->size();++i)
      if((*modules)[i])painter.drawRect(int(i)*module-bounds.left(),-bounds.top(),module,height);
    for(const auto& glyph:glyphs)paintPrinterGlyph(painter,glyph.baseline-bounds.topLeft(),glyph.glyph,options.foreground);
    painter.end();
    QPoint position=state.position;
    if(state.baseline)position-=rotatedEdge(QPoint(-bounds.left(),height-bounds.top()),symbol.size(),code.orientation);
    else switch(code.orientation){
      case Orientation::Normal:position+=bounds.topLeft();break;
      case Orientation::Rotated90:position+=QPoint(height-bounds.bottom()-1,bounds.left());break;
      case Orientation::Inverted:position+=QPoint(barsWidth-bounds.right()-1,height-bounds.bottom()-1);break;
      case Orientation::BottomUp:position+=QPoint(bounds.top(),barsWidth-bounds.right()-1);break;
    }
    const auto rotated=rotateImage(symbol,code.orientation);
    observeRaster(state,position,rotated);
    QPainter target(&image);target.drawImage(position,rotated);
    return;
  }
  const int gap=interpretation?5:0;
  QImage caption;
  if(interpretation){
    const auto captionData=BarcodeEncoders::Detail::code128UccData(text,mode,uccCheck);
    const QString interpretationText=code128Interpretation(captionData?*captionData:text);
    // The default interpretation scales with ^BY's module width. An ^A in
    // this field may override it; an ^A belonging to a previous ^FS field may
    // not leak into the barcode caption.
    const auto captionFont=state.barcodeInterpretationFont;
    if(captionFont&&(captionFont->height>32000||captionFont->width>32000)){rasterAllowed(32001,1);return;}
    QImage field;
    if(captionFont&&captionFont->font==u'0'){
      const Font0Metrics metrics(std::max(1,captionFont->height),captionFont->width,fonts,{},code.orientation);
      const auto outline=metrics.outline(interpretationText);
      const QRect bounds=outline.boundingRect().toAlignedRect();
      if(!rasterAllowed(std::max(1,bounds.width()),std::max(1,bounds.height())))return;
      field=QImage(std::max(1,bounds.width()),std::max(1,bounds.height()),QImage::Format_ARGB32_Premultiplied);
      field.fill(Qt::transparent);
      QPainter painter(&field);painter.setRenderHint(QPainter::Antialiasing,false);
      painter.fillPath(outline.translated(-bounds.left(),-bounds.top()),options.foreground);
    }else{
      const QFont font=captionFont
        ? builtInFont(captionFont->font,captionFont->height,captionFont->width)
        : builtInFont(u'A',module*10);
      const QRect bounds=QFontMetrics(font).boundingRect(interpretationText).adjusted(-1,-1,1,1);
      if(!rasterAllowed(std::max(1,bounds.width()),std::max(1,bounds.height())))return;
      field=QImage(std::max(1,bounds.width()),std::max(1,bounds.height()),QImage::Format_ARGB32_Premultiplied);
      field.fill(Qt::transparent);
      QPainter painter(&field);painter.setRenderHint(QPainter::TextAntialiasing,false);
      painter.setFont(font);painter.setPen(options.foreground);
      painter.drawText(-bounds.left(),-bounds.top(),interpretationText);
    }
    const QRect ink=alphaBounds(field);
    if(!ink.isEmpty())caption=field.copy(ink);
  }
  const int captionHeight=caption.height();
  const int barsTop=interpretationAbove?captionHeight+gap:0;
  const int symbolHeight=height+captionHeight+gap;
  if(!rasterAllowed(qint64(modules->size())*module,symbolHeight))return;
  QImage symbol(modules->size()*module,symbolHeight,QImage::Format_ARGB32_Premultiplied);symbol.fill(options.background);
  QPainter painter(&symbol);painter.setRenderHint(QPainter::Antialiasing,false);painter.setPen(Qt::NoPen);painter.setBrush(options.foreground);
  for(qsizetype i=0;i<modules->size();++i)if((*modules)[i])painter.drawRect(static_cast<int>(i)*module,barsTop,module,height);
  if(!caption.isNull()){
    const int textTop=interpretationAbove?0:height+gap;
    const int textLeft=state.barcodeInterpretationFont?0:(symbol.width()-caption.width())/2;
    painter.drawImage(textLeft,textTop,caption);
  }
  painter.end();
  const QSize sourceSize=symbol.size();
  symbol=rotateImage(symbol,code.orientation);
  QPoint position=state.position;
  if(state.baseline){
    const int base=barsTop+height;
    switch(code.orientation){
      case Orientation::Normal:position.ry()-=base;break;
      case Orientation::Rotated90:position.rx()-=sourceSize.height()-base;break;
      case Orientation::Inverted:position-=QPoint(sourceSize.width(),sourceSize.height()-base);break;
      case Orientation::BottomUp:position-=QPoint(base,sourceSize.width());break;
    }
  }else if(code.orientation==Orientation::Normal)position.ry()-=barsTop;
  observeRaster(state,position,symbol);
  QPainter target(&image);target.drawImage(position,symbol);
}

void drawCodabar(QImage& image,State& state,const Barcode& barcode,const ResolvedBarcode& code,const QString& text,const RenderOptions& options,QList<Diagnostic>& diagnostics) {
  const auto& codabar=std::get<LinearOptions>(code.options);
  // Like Labelary, encode only the data characters; ^BK supplies start/stop.
  QString data=text;
  data.removeIf([](QChar c){return !QStringView{u"0123456789-$:/.+"}.contains(c);});
  if(data.size()!=text.size())diagnostics.append({Severity::Warning,u"codabar-data-ignored"_s,
    u"Codabar field characters other than 0-9 - $ : / . + were dropped; ^BK k and l supply the start and stop characters."_s,
    -1,u"^BK"_s});
  // Labelary prints no symbol when nothing is left to encode.
  if(data.isEmpty()){diagnostics.append({Severity::Error,u"codabar-encode"_s,u"Codabar field has no data characters to encode."_s,-1,u"^BK"_s});return;}
  auto modules=BarcodeEncoders::codabar(codabar.start+data+codabar.stop,int(code.wideToNarrowRatio));
  if(!modules){diagnostics.append({Severity::Error,u"codabar-encode"_s,modules.error(),-1,u"^BK"_s});return;}
  const int module=code.moduleWidth;
  const int height=code.height;
  if(!barcodeRasterAllowed(qint64(modules->size())*module,height,barcode,diagnostics))return;
  QImage symbol(modules->size()*module,height,QImage::Format_ARGB32_Premultiplied);symbol.fill(options.background);
  QPainter painter(&symbol);painter.setPen(Qt::NoPen);painter.setBrush(options.foreground);
  for(qsizetype i=0;i<modules->size();++i)if((*modules)[i])painter.drawRect(static_cast<int>(i)*module,0,module,height);
  painter.end();placeNewSymbol(image,symbol,state,code.orientation);
}

void drawPdf417(QImage& image,State& state,const ResolvedBarcode& code,const QByteArray& text,const RenderOptions& options,QList<Diagnostic>& diagnostics) {
  const auto& pdf=std::get<Pdf417Options>(code.options);
  const int module=code.moduleWidth;
  const int rowHeight=code.height;
  if(text.size()>2710){diagnostics.append({Severity::Error,u"pdf417-encode"_s,u"PDF417 data exceeds the maximum capacity."_s,-1,u"^B7"_s});return;}
  auto symbolData=BarcodeEncoders::Pdf417::encode(text,pdf.security,pdf.columns,pdf.rows,pdf.truncated);
  if(!symbolData){diagnostics.append({Severity::Error,u"pdf417-encode"_s,symbolData.error(),-1,u"^B7"_s});return;}
  const auto& matrix=symbolData->matrix;
  if(qint64(matrix.width)*module*matrix.height*rowHeight>64*1024*1024){
    diagnostics.append({Severity::Error,u"barcode-size"_s,u"Invalid or unsafe PDF417 row height."_s,-1,u"^B7"_s});return;
  }
  QImage symbol(matrix.width*module,matrix.height*rowHeight,QImage::Format_ARGB32_Premultiplied);symbol.fill(options.background);
  QPainter painter(&symbol);painter.setPen(Qt::NoPen);painter.setBrush(options.foreground);
  for(int y=0;y<matrix.height;++y)for(int x=0;x<matrix.width;++x)if(matrix.at(x,y))painter.drawRect(x*module,y*rowHeight,module,rowHeight);
  painter.end();placeNewSymbol(image,symbol,state,code.orientation);
}

// MaxiCode geometry follows OkapiBarcode's MaxiCode/Hexagon plot elements.
// Copyright 2014-2015 Robin Stuart, Daniel Gredler and Robert Elliott.
// Apache-2.0; see third_party/okapi-maxicode/NOTICE and LICENSE.
// Raster conversion is native, deterministic and expressed in printer dots.
void fillMaxiCodePolygon(QImage& image,const QVector<QPointF>& points,QRgb colour,bool subpixel=false) {
  double minimumY=points.front().y(),maximumY=minimumY;
  for(const auto& point:points){minimumY=std::min(minimumY,point.y());maximumY=std::max(maximumY,point.y());}
  const int firstY=std::max(0,static_cast<int>(std::ceil(minimumY)));
  const int lastY=std::min(image.height(),static_cast<int>(std::ceil(maximumY)));
  for(int y=firstY;y<lastY;++y){
    double left=std::numeric_limits<double>::max(),right=std::numeric_limits<double>::lowest();
    for(qsizetype i=0;i<points.size();++i){
      const auto& a=points[i];const auto& b=points[(i+1)%points.size()];
      if(y<std::min(a.y(),b.y())||y>=std::max(a.y(),b.y()))continue;
      const double x=a.x()+(b.x()-a.x())*(y-a.y())/(b.y()-a.y());
      left=std::min(left,x);right=std::max(right,x);
    }
    if(left>right)continue;
    // The finder uses 8-bit subpixel scanline boundaries. Hexagon vertices
    // have already been snapped to integer dots, before scan conversion.
    if(subpixel){left=std::floor(left*256.0)/256.0;right=std::floor(right*256.0)/256.0;}
    const int firstX=std::max(0,static_cast<int>(std::ceil(left)));
    const int lastX=std::min(image.width(),static_cast<int>(std::ceil(right)));
    auto* scanline=reinterpret_cast<QRgb*>(image.scanLine(y));
    std::fill(scanline+firstX,scanline+lastX,colour);
  }
}

QVector<QPointF> maxiCodeFinderCircle(double centerX,double centerY,double radius) {
  // Standard four-cubic circle, eight line segments per quadrant at this
  // fixed dot size. Sampling the analytic circle changes boundary pixels.
  constexpr double control=0.5522847498307933;
  QVector<QPointF> points;points.reserve(32);
  for(int quadrant=0;quadrant<4;++quadrant)for(int step=0;step<8;++step){
    const double t=step/8.0,u=1.0-t;
    double x=u*u*u+3.0*u*u*t+3.0*u*t*t*control;
    double y=3.0*u*u*t*control+3.0*u*t*t+t*t*t;
    for(int turn=0;turn<quadrant;++turn){const double previousX=x;x=-y;y=previousX;}
    points.append(QPointF(centerX+radius*x,centerY+radius*y));
  }
  return points;
}

QImage rasterizeMaxiCode(const BarcodeEncoders::Matrix& grid,const RenderOptions& options) {
  // The fixed MaxiCode pitch is independent of display/printer DPI, matching
  // the 203-DPI dot geometry used by Labelary's ^BD command at every density.
  constexpr double scale=2.72;
  constexpr int width=static_cast<int>(74*scale),height=static_cast<int>(72*scale);
  QImage symbol(width,height,QImage::Format_ARGB32_Premultiplied);symbol.fill(options.background);
  const QRgb ink=qPremultiply(options.foreground.rgba()),paper=qPremultiply(options.background.rgba());
  constexpr double offsetX[]{0.0,0.86,0.86,0.0,-0.86,-0.86};
  constexpr double offsetY[]{1.0,0.5,-0.5,-1.0,-0.5,0.5};
  QVector<QPointF> points;points.reserve(6);
  for(int row=0;row<33;++row)for(int column=0;column<30-(row&1);++column){
    if(!grid.at(column,row))continue;
    const double centerX=(2.46*column+1.23+((row&1)?1.23:0.0))*scale;
    const double centerY=(2.135*row+1.43)*scale;
    points.clear();
    for(int vertex=0;vertex<6;++vertex)
      points.append(QPointF(static_cast<int>(centerX+offsetX[vertex]*1.25*scale),
                           static_cast<int>(centerY+offsetY[vertex]*1.25*scale)));
    fillMaxiCodePolygon(symbol,points,ink);
  }
  constexpr double radii[]{10.85,8.97,7.10,5.22,3.31,1.43};
  for(int ring=0;ring<6;++ring)
    fillMaxiCodePolygon(symbol,maxiCodeFinderCircle(35.76*scale,35.60*scale,radii[ring]*scale),
                        (ring&1)?paper:ink,true);
  return symbol;
}

void drawMaxiCode(QImage& image,State& state,const ResolvedBarcode& code,const QByteArray& text,const RenderOptions& options,QList<Diagnostic>& diagnostics) {
  const auto& maxi=std::get<MaxiCodeOptions>(code.options);
  auto encoded=BarcodeEncoders::MaxiCode::encode(text,maxi.mode,maxi.position,maxi.total);
  if(!encoded){diagnostics.append({Severity::Error,u"maxicode-encode"_s,encoded.error(),-1,u"^BD"_s});return;}
  QImage symbol=rotateImage(rasterizeMaxiCode(encoded->grid,options),code.orientation);
  QPoint position=state.position;if(state.baseline)position.ry()-=symbol.height();
  observeRaster(state,position,symbol);
  QPainter target(&image);target.drawImage(position,symbol);
}

int barcodeParameter(const Barcode& barcode,qsizetype index,int fallback) {
  if(index>=barcode.parameters.size()||barcode.parameters[index].isEmpty())return fallback;
  bool ok=false;const int value=barcode.parameters[index].toInt(&ok);return ok?value:fallback;
}

void placeNewSymbol(QImage& image,const QImage& original,const State& state,Orientation orientation) {
  QPoint position=state.position;
  if(state.baseline)position-=rotatedEdge(QPoint(state.justification==Justification::Right?original.width():0,original.height()),original.size(),orientation);
  else if(state.justification==Justification::Right)position.rx()-=(orientation==Orientation::Rotated90||orientation==Orientation::BottomUp?original.height():original.width());
  const auto rotated=rotateImage(original,orientation);
  observeRaster(state,position,rotated);
  QPainter painter(&image);painter.drawImage(position,rotated);
}

void drawExtendedLinear(QImage& image,State& state,const Barcode& barcode,const ResolvedBarcode& code,const QString& data,const RenderOptions& options,QList<Diagnostic>& diagnostics,RenderFonts& fonts) {
  const int module=code.moduleWidth;
  const int height=code.height;
  const bool interleaved=barcode.symbology==u"B2",industrial=barcode.symbology==u"BI";
  const bool ean13=barcode.symbology==u"BE";
  const bool retail=ean13||barcode.symbology==u"B8"||barcode.symbology==u"B9"||barcode.symbology==u"BU";
  const bool caption=code.interpretation;
  const bool above=code.interpretationAbove;
  // UPC-A/UPC-E print their check digit; Code 93 and I2of5 add one on request.
  const bool check=std::get<LinearOptions>(code.options).checkDigit;
  const int wide=std::clamp(static_cast<int>(module*code.wideToNarrowRatio),module,30);
  auto encoded=[&]()->std::expected<BarcodeEncoders::Linear::Symbol,QString>{
    if(!ean13)return BarcodeEncoders::Linear::encode(barcode.symbology,data,module,wide,interleaved&&check);
    BarcodeEncoders::Linear::Symbol symbol;
    auto modules=BarcodeEncoders::ean13(data,&symbol.text);
    if(!modules)return std::unexpected(modules.error());
    symbol.modules=std::move(*modules);
    return symbol;
  }();
  if(!encoded){diagnostics.append({Severity::Error,ean13?u"ean13-encode"_s:u"barcode-encode"_s,encoded.error(),-1,u'^'+barcode.symbology});return;}
  const int scale=interleaved||industrial?1:module;
  const int barsWidth=static_cast<int>(encoded->modules.size())*scale;
  // Labelary extends EAN/UPC guard bars by 13 dots at every module width.
  const int guardExtra=retail?13:0;
  // Room for the tallest interpretation glyph on either side of the bars;
  // anchors below subtract it, so it never moves the bar body.
  const int textHeight=caption?10*module+12:0;
  const int sidePad=caption&&retail&&barcode.symbology!=u"B8"?14*module:0;
  const int barsTop=caption&&above?textHeight:0;
  if(qint64(barsWidth+2*sidePad)*qint64(height+guardExtra+textHeight)>64*1024*1024){diagnostics.append({Severity::Error,u"barcode-size"_s,u"Barcode raster exceeds the allocation limit."_s,-1,u'^'+barcode.symbology});return;}
  QImage symbol(barsWidth+2*sidePad,height+guardExtra+textHeight,QImage::Format_ARGB32_Premultiplied);symbol.fill(Qt::transparent);
  QPainter painter(&symbol);painter.setPen(Qt::NoPen);painter.setBrush(options.foreground);
  for(qsizetype i=0;i<encoded->modules.size();++i)if(encoded->modules[i]){
    const int count=static_cast<int>(encoded->modules.size());
    const int outsideGuard=barcode.symbology==u"BU"?10:3;
    const bool guard=retail&&(i<outsideGuard||i>=count-(barcode.symbology==u"B9"?6:outsideGuard)||(barcode.symbology!=u"B9"&&i>=count/2-2&&i<=count/2+2));
    painter.drawRect(sidePad+static_cast<int>(i)*scale,barsTop,scale,height+(guard?guardExtra:0));
  }
  if(caption){
    const QString text=encoded->text;
    if(retail&&!above){
      // Font A below ^BY3, then the native OCR-B sizes, for EAN and UPC alike.
      const bool fontA=module<3;
      // Two OCR-B sizes cover ^BY3..10; EAN-13 alone accepts wider modules.
      const int fontScale=std::clamp(module/4,1,2);
      const int cellWidth=fontA?6*module:20*fontScale;
      const int baseline=height+(fontA?7*module+4:23*fontScale+4);
      const auto print=[&](int origin,QStringView part,bool grouped=true){
        const int count=static_cast<int>(part.size());
        const qreal pitch=grouped&&count>1?qreal((7*count-3)*module-cellWidth)/(count-1):cellWidth;
        for(int character=0;character<count;++character){
          const int x=sidePad+qRound(origin+character*pitch);
          if(fontA){
            paintPrinterFontAGlyph(painter,QPoint(x,baseline),part[character],9*module,5*module,options.foreground,fonts,code.orientation);
          }else{
            const auto& glyph=RetailFont::digits[fontScale-1][part[character].unicode()-u'0'];
            for(int row=0;row<glyph.height;++row){
              const auto bits=RetailFont::rowBits(fontScale-1,part[character].unicode()-u'0',static_cast<char>(code.orientation),row);
              for(int column=0;column<glyph.width;){
                if(!(bits&(std::uint32_t{1}<<column))){++column;continue;}
                const int start=column;
                while(column<glyph.width&&(bits&(std::uint32_t{1}<<column)))++column;
                painter.fillRect(x+glyph.xOffset+start,baseline+glyph.yOffset+row,column-start,1,options.foreground);
              }
            }
          }
        }
      };
      if(barcode.symbology==u"B8"){
        print(5*module,QStringView{text}.first(4));print(37*module,QStringView{text}.last(4));
      }else if(ean13){
        print(-5*module-cellWidth,QStringView{text}.first(1),false);
        print(5*module,QStringView{text}.mid(1,6));print(51*module,QStringView{text}.mid(7,6));
      }else{
        print(-5*module-cellWidth,QStringView{text}.first(1),false);
        if(barcode.symbology==u"B9")print(5*module,QStringView{text}.mid(1,6));
        else{print(12*module,QStringView{text}.mid(1,5));print(51*module,QStringView{text}.mid(6,5));}
        if(check)print(barsWidth+2*module,QStringView{text}.last(1),false);
      }
    }else{
      QString interpretation=text;
      if(barcode.symbology==u"BA"){if(check)interpretation+=encoded->checkText;interpretation=QChar(0x25af)+interpretation+QChar(0x25af);}
      const qreal advance=qreal(9*module)*1365/2048;
      const qreal first=sidePad+(barsWidth-(retail?module:0)-advance*interpretation.size())/2.0;
      // The Font A line sits where Code 128 puts its interpretation.
      const int baseline=above?barsTop-(2*module+8):barsTop+height+7*module+6;
      for(qsizetype character=0;character<interpretation.size();++character){
        const qreal origin=first+character*advance;
        // A half-dot origin rounds in the direction of the transformed axis.
        const int x=code.orientation==Orientation::Inverted||code.orientation==Orientation::BottomUp
          ?static_cast<int>(std::ceil(origin-0.5)):qRound(origin);
        paintPrinterFontAGlyph(painter,QPoint(x,baseline),interpretation[character],9*module,5*module,options.foreground,fonts,code.orientation);
      }
    }
  }
  painter.end();
  State placed=state;
  if(state.baseline){
    // ^FT anchors the lower edge of the first bar, excluding the caption
    // and the retail guard extensions, before applying the orientation.
    const QPoint anchor(sidePad+(state.justification==Justification::Right?barsWidth:0),barsTop+height);
    placed.position-=rotatedEdge(anchor,symbol.size(),code.orientation);
    placed.baseline=false;
    placed.justification=Justification::Left;
  }else{
    // ^FO names the top-left corner of the rotated bar body. Caption space
    // can occur on either side after rotation, but must not move that body.
    switch(code.orientation){
      case Orientation::Normal:placed.position-=QPoint(sidePad,barsTop);break;
      case Orientation::Rotated90:placed.position-=QPoint(symbol.height()-barsTop-height,sidePad);break;
      case Orientation::Inverted:placed.position-=QPoint(sidePad,symbol.height()-barsTop-height);break;
      case Orientation::BottomUp:placed.position-=QPoint(barsTop,sidePad);break;
    }
  }
  placeNewSymbol(image,symbol,placed,code.orientation);
}

void drawExtendedMatrix(QImage& image,State& state,const Barcode& barcode,const ResolvedBarcode& code,const FieldData& field,const RenderOptions& options,QList<Diagnostic>& diagnostics) {
  using BarcodeEncoders::Matrix;
  std::expected<Matrix,QString> matrix=std::unexpected(u"Unsupported matrix barcode options"_s);
  const int module=code.moduleWidth;
  int rowHeight=module;
  const auto& data=field.data;
  if(barcode.symbology==u"BR"&&std::ranges::any_of(data,[](QChar c){return c.unicode()>255;})){
    diagnostics.append({Severity::Error,u"barcode-encoding"_s,u"This barcode requires explicit byte data; use ^FH for binary fields."_s,-1,u'^'+barcode.symbology});return;
  }
  if(const auto* micro=std::get_if<MicroPdf417Options>(&code.options)){
    matrix=BarcodeEncoders::MicroPdf417::encode(*field.bytes,micro->mode);
    rowHeight=code.height;
  }else if(const auto* dataBar=std::get_if<DataBarOptions>(&code.options)){
    matrix=BarcodeEncoders::DataBar::encode(data,dataBar->type,code.height,dataBar->segments);
  }else if(const auto* aztec=std::get_if<AztecOptions>(&code.options)){
    if(aztec->eci||aztec->structuredAppend!=1){matrix=std::unexpected(u"Aztec embedded ECI and structured append are not supported."_s);}
    else matrix=BarcodeEncoders::Aztec::encode(*field.bytes,aztec->size,aztec->readerInitialization);
  }
  if(!matrix){diagnostics.append({Severity::Error,u"barcode-encode"_s,matrix.error(),-1,u'^'+barcode.symbology});return;}
  if(module<1||module>10||rowHeight<1||rowHeight>9999||qint64(matrix->width)*module*matrix->height*rowHeight>64*1024*1024){diagnostics.append({Severity::Error,u"barcode-size"_s,u"Invalid or unsafe barcode module size."_s,-1,u'^'+barcode.symbology});return;}
  QImage symbol(matrix->width*module,matrix->height*rowHeight,QImage::Format_ARGB32_Premultiplied);symbol.fill(options.background);
  QPainter painter(&symbol);painter.setPen(Qt::NoPen);painter.setBrush(options.foreground);
  for(int y=0;y<matrix->height;++y)for(int x=0;x<matrix->width;++x)if(matrix->at(x,y))painter.drawRect(x*module,y*rowHeight,module,rowHeight);
  painter.end();placeNewSymbol(image,symbol,state,code.orientation);
}

Symbology symbologyOf(QStringView opcode) {
  struct Entry { QStringView opcode; Symbology symbology; };
  static constexpr Entry entries[]{
    {u"BC",Symbology::Code128},{u"B3",Symbology::Code39},{u"BA",Symbology::Code93},{u"BK",Symbology::Codabar},
    {u"BE",Symbology::Ean13},{u"B8",Symbology::Ean8},{u"BU",Symbology::UpcA},{u"B9",Symbology::UpcE},
    {u"B2",Symbology::Interleaved2of5},{u"BI",Symbology::Industrial2of5},{u"BR",Symbology::DataBar},
    {u"BQ",Symbology::QrCode},{u"BX",Symbology::DataMatrix},{u"B7",Symbology::Pdf417},
    {u"BF",Symbology::MicroPdf417},{u"BO",Symbology::Aztec},{u"B0",Symbology::Aztec},{u"BD",Symbology::MaxiCode}};
  for(const auto& entry:entries)if(entry.opcode==opcode)return entry.symbology;
  return Symbology::Unknown;
}

bool hasParameter(const Barcode& barcode,qsizetype index) {
  return index<barcode.parameters.size()&&!barcode.parameters[index].isEmpty();
}

// Labelary reads y/n flags like Y/N in every barcode command.
bool flagYes(const Barcode& barcode,qsizetype index) {
  return index<barcode.parameters.size()&&barcode.parameters[index].compare(u"Y",Qt::CaseInsensitive)==0;
}

bool flagNotNo(const Barcode& barcode,qsizetype index) {
  return index>=barcode.parameters.size()||barcode.parameters[index].compare(u"N",Qt::CaseInsensitive)!=0;
}

bool flagValid(const Barcode& barcode,qsizetype index) {
  return !hasParameter(barcode,index)||flagYes(barcode,index)||!flagNotNo(barcode,index);
}

} // namespace

// The one interpretation of barcode parameters, ^BY state and ^FW for both
// rendering and analysis. Errors are rendering diagnostics for the command;
// data-dependent checks stay with the encoders.
std::expected<ResolvedBarcode,Diagnostic> resolveBarcode(const Barcode& barcode,const State& state) {
  const auto error=[&](QString code,QString message){
    return std::unexpected(Diagnostic{Severity::Error,std::move(code),std::move(message),-1,u'^'+barcode.symbology});
  };
  const auto invalidNumber=[&](std::initializer_list<int> indices)->std::optional<Diagnostic>{
    for(const int index:indices){
      if(!hasParameter(barcode,index))continue;
      bool ok=false;(void)barcode.parameters[index].toInt(&ok);
      if(!ok)return Diagnostic{Severity::Error,u"barcode-parameter"_s,
        u"Barcode parameter %1 must be an integer."_s.arg(index+1),-1,u'^'+barcode.symbology};
    }
    return std::nullopt;
  };
  const auto& defaults=state.barcodeDefaults;
  ResolvedBarcode code;
  code.symbology=symbologyOf(barcode.symbology);
  code.orientationOmitted=barcode.orientationOmitted;
  code.orientation=barcode.orientationOmitted?state.barcodeDirection:barcode.orientation;
  code.wideToNarrowRatio=defaults.wideToNarrowRatio;
  switch(code.symbology){
    case Symbology::Code128:{
      const QChar mode=hasParameter(barcode,5)?barcode.parameters[5].front():u'N';
      code.moduleWidth=std::clamp(defaults.moduleWidth,1,10);
      code.height=hasParameter(barcode,1)?std::max(1,barcode.parameters[1].toInt()):defaults.height;
      code.interpretation=flagNotNo(barcode,2);
      code.interpretationAbove=hasParameter(barcode,3)?flagYes(barcode,3):mode.toUpper()==u'U';
      code.options=Code128Options{mode,flagYes(barcode,4)};
      break;
    }
    case Symbology::Code39:{
      const int narrow=std::max(1,defaults.moduleWidth);
      if(narrow>32000||!std::isfinite(defaults.wideToNarrowRatio)||narrow*defaults.wideToNarrowRatio>32000)
        return error(u"barcode-size"_s,u"Code 39 module dimensions exceed the allocation limit."_s);
      code.moduleWidth=narrow;
      code.height=std::max(1,hasParameter(barcode,2)?barcode.parameters[2].toInt():defaults.height);
      code.interpretation=flagNotNo(barcode,3);
      code.interpretationAbove=flagYes(barcode,4);
      code.options=LinearOptions{flagYes(barcode,1)};
      break;
    }
    case Symbology::Codabar:{
      code.wideToNarrowRatio=std::clamp(qRound(defaults.wideToNarrowRatio),2,3);
      code.moduleWidth=std::clamp(defaults.moduleWidth,1,10);
      code.height=hasParameter(barcode,2)?std::max(1,barcode.parameters[2].toInt()):defaults.height;
      code.interpretation=flagNotNo(barcode,3);
      code.interpretationAbove=flagYes(barcode,4);
      // Labelary takes start/stop from k and l in any case and uses A for an
      // invalid character.
      const auto startStop=[&](qsizetype index){
        const QString value=index<barcode.parameters.size()?barcode.parameters[index].toUpper():QString();
        return value.size()==1&&QStringView{u"ABCD"}.contains(value.front())?value.front():QChar(u'A');
      };
      code.options=LinearOptions{flagYes(barcode,1),startStop(5),startStop(6)};
      break;
    }
    case Symbology::Ean13:
      code.moduleWidth=std::max(1,defaults.moduleWidth);
      if(code.moduleWidth>32000)return error(u"barcode-size"_s,u"EAN-13 module dimensions exceed the allocation limit."_s);
      code.height=hasParameter(barcode,1)?std::max(1,barcode.parameters[1].toInt()):defaults.height;
      code.interpretation=flagNotNo(barcode,2);
      code.interpretationAbove=flagYes(barcode,3);
      code.options=LinearOptions{};
      break;
    case Symbology::Ean8:case Symbology::UpcA:case Symbology::UpcE:
    case Symbology::Code93:case Symbology::Interleaved2of5:case Symbology::Industrial2of5:{
      code.moduleWidth=std::clamp(defaults.moduleWidth,1,10);
      code.height=barcodeParameter(barcode,1,defaults.height);
      if(code.height<1||code.height>32000)return error(u"barcode-height"_s,u"Barcode height must be 1 through 32000 dots."_s);
      if(!std::isfinite(defaults.wideToNarrowRatio)||defaults.wideToNarrowRatio<2.0||defaults.wideToNarrowRatio>3.0)
        return error(u"barcode-ratio"_s,u"Barcode wide-to-narrow ratio must be finite and between 2 and 3."_s);
      code.interpretation=flagNotNo(barcode,2);
      code.interpretationAbove=flagYes(barcode,3);
      const bool upc=code.symbology==Symbology::UpcA||code.symbology==Symbology::UpcE;
      code.options=LinearOptions{upc?flagNotNo(barcode,4):flagYes(barcode,4)};
      break;
    }
    case Symbology::QrCode:{
      const int model=hasParameter(barcode,1)?barcode.parameters[1].toInt():2;
      if(model!=2)return error(u"qrcode-model"_s,u"Only QR Model 2 is implemented; no symbol was rendered."_s);
      code.moduleWidth=qrMagnification(barcode);
      code.options=QrCodeOptions{model};
      break;
    }
    case Symbology::DataMatrix:{
      if(const auto invalid=invalidNumber({1,2,3,4,5,7}))return std::unexpected(*invalid);
      if(hasParameter(barcode,2)&&barcode.parameters[2].toInt()!=200)
        return error(u"datamatrix-quality"_s,u"Only DataMatrix ECC 200 is implemented; no symbol was rendered."_s);
      const int ratio=hasParameter(barcode,7)?barcode.parameters[7].toInt():1;
      if(ratio!=1&&ratio!=2)return error(u"datamatrix-ratio"_s,u"DataMatrix aspect ratio must be 1 (square) or 2 (rectangular)."_s);
      DataMatrixOptions matrix;
      matrix.escape=hasParameter(barcode,6)?barcode.parameters[6].front():u'~';
      if(matrix.escape.unicode()>255)return error(u"datamatrix-encoding"_s,u"DataMatrix requires a single-byte escape character."_s);
      code.moduleWidth=hasParameter(barcode,1)?barcode.parameters[1].toInt():3;
      if(code.moduleWidth<1||code.moduleWidth>32000)return error(u"barcode-size"_s,u"Invalid or unsafe DataMatrix module size."_s);
      matrix.columns=hasParameter(barcode,3)?barcode.parameters[3].toInt():0;
      matrix.rows=hasParameter(barcode,4)?barcode.parameters[4].toInt():0;
      matrix.format=hasParameter(barcode,5)?barcode.parameters[5].toInt():0;
      matrix.gs1=matrix.format==1;
      matrix.rectangular=ratio==2;
      code.options=matrix;
      break;
    }
    case Symbology::Pdf417:{
      if(const auto invalid=invalidNumber({1,2,3,4}))return std::unexpected(*invalid);
      if(!flagValid(barcode,5))return error(u"barcode-parameter"_s,u"PDF417 truncation must be Y or N."_s);
      code.moduleWidth=std::clamp(defaults.moduleWidth,1,100);
      code.height=hasParameter(barcode,1)?barcode.parameters[1].toInt():code.moduleWidth*3;
      if(code.height<1)return error(u"barcode-size"_s,u"Invalid or unsafe PDF417 row height."_s);
      code.options=Pdf417Options{hasParameter(barcode,2)?barcode.parameters[2].toInt():0,
        hasParameter(barcode,3)?barcode.parameters[3].toInt():0,
        hasParameter(barcode,4)?barcode.parameters[4].toInt():0,flagYes(barcode,5)};
      break;
    }
    case Symbology::MicroPdf417:{
      if(const auto invalid=invalidNumber({1,2}))return std::unexpected(*invalid);
      const int mode=barcodeParameter(barcode,2,0);
      const int rows=BarcodeEncoders::MicroPdf417::rows(mode);
      if(rows<1)return error(u"barcode-encode"_s,u"MicroPDF417 mode must be between 0 and 33"_s);
      code.moduleWidth=std::clamp(defaults.moduleWidth,1,10);
      // The mode fixes the row count; the ^BY height spans all rows.
      code.height=barcodeParameter(barcode,1,std::max(1,defaults.height/rows));
      if(code.height<1||code.height>9999)return error(u"barcode-size"_s,u"Invalid or unsafe barcode module size."_s);
      code.options=MicroPdf417Options{mode};
      break;
    }
    case Symbology::DataBar:{
      if(const auto invalid=invalidNumber({1,2,3,4,5}))return std::unexpected(*invalid);
      const int separator=barcodeParameter(barcode,3,1);
      code.height=barcodeParameter(barcode,4,25);
      if(separator<1||separator>2||code.height<1||code.height>32000)
        return error(u"barcode-parameter"_s,u"GS1 DataBar requires separator height 1 or 2 and bar height 1 through 32000."_s);
      code.moduleWidth=barcodeParameter(barcode,2,2);
      if(code.moduleWidth<1||code.moduleWidth>10)return error(u"barcode-size"_s,u"Invalid or unsafe barcode module size."_s);
      code.options=DataBarOptions{barcodeParameter(barcode,1,1),separator,barcodeParameter(barcode,5,0)};
      break;
    }
    case Symbology::Aztec:{
      if(const auto invalid=invalidNumber({1,3,5}))return std::unexpected(*invalid);
      if(!flagValid(barcode,2)||!flagValid(barcode,4))
        return error(u"barcode-parameter"_s,u"Aztec ECI and reader initialization flags must be Y or N."_s);
      code.moduleWidth=barcodeParameter(barcode,1,2);
      if(code.moduleWidth<1||code.moduleWidth>10)return error(u"barcode-size"_s,u"Invalid or unsafe barcode module size."_s);
      // Labelary ignores a size outside 1-99/101-104/201-232/300, including
      // Zebra's 0 (default error correction), and draws the default symbol.
      int size=barcodeParameter(barcode,3,23);
      if(!((size>=1&&size<=99)||(size>=101&&size<=104)||(size>=201&&size<=232)||size==300))size=23;
      code.options=AztecOptions{size,flagYes(barcode,2),flagYes(barcode,4),barcodeParameter(barcode,5,1)};
      break;
    }
    case Symbology::MaxiCode:
      // MaxiCode has fixed geometry and no orientation parameter.
      code.orientation=Orientation::Normal;code.orientationOmitted=false;
      code.options=MaxiCodeOptions{hasParameter(barcode,0)?barcode.parameters[0].toInt():2,
        hasParameter(barcode,1)?barcode.parameters[1].toInt():1,hasParameter(barcode,2)?barcode.parameters[2].toInt():1};
      break;
    case Symbology::Unknown:break;
  }
  return code;
}

void drawBarcode(QImage& image, State& state, const Barcode& barcode, const FieldData& field, const RenderOptions& options, QList<Diagnostic>& diagnostics,RenderFonts& fonts) {
  if(state.reverse){
    QImage mask(image.size(),QImage::Format_ARGB32_Premultiplied);mask.fill(Qt::transparent);
    RenderOptions ink=options;ink.foreground=Qt::white;ink.background=Qt::transparent;
    state.reverse=false;drawBarcode(mask,state,barcode,field,ink,diagnostics,fonts);
    QPainter painter(&image);painter.setCompositionMode(QPainter::CompositionMode_Difference);painter.drawImage(0,0,mask);return;
  }
  const auto& data=field.data;
  const bool requiresBytes=barcode.symbology==u"BQ"||barcode.symbology==u"BX"||barcode.symbology==u"B7"
    ||barcode.symbology==u"BD"||barcode.symbology==u"BF"||barcode.symbology==u"BO"||barcode.symbology==u"B0";
  const auto code=resolveBarcode(barcode,state);
  if(requiresBytes&&!field.bytes){
    const QString diagnosticCode=barcode.symbology==u"BQ"?u"qrcode-encoding"_s
      :barcode.symbology==u"BX"?u"datamatrix-encoding"_s
      :barcode.symbology==u"B7"?u"pdf417-encoding"_s
      :barcode.symbology==u"BD"?u"maxicode-encode"_s:u"barcode-encoding"_s;
    diagnostics.append({Severity::Error,diagnosticCode,
      u"The field cannot be represented as bytes in the selected character set; use ^CI28 for Unicode or ^FH for binary bytes."_s,
      -1,u'^'+barcode.symbology});
  }
  else if(!code)diagnostics.append(code.error());
  else if(barcode.symbology==u"B3"||barcode.symbology==u"BK"){
    // The requested human-readable line is not drawn yet; keep the symbol.
    if(code->interpretation)diagnostics.append({Severity::Warning,u"barcode-interpretation-pending"_s,
      u"The interpretation line of this barcode is not drawn yet; the symbol is rendered without it."_s,-1,u'^'+barcode.symbology});
    if(barcode.symbology==u"B3")drawCode39(image,state,barcode,*code,data,options,diagnostics);
    else drawCodabar(image,state,barcode,*code,data,options,diagnostics);
  }
  else if(barcode.symbology==u"BQ")drawQrCode(image,state,barcode,*code,*field.bytes,options,diagnostics);
  else if(barcode.symbology==u"BX")drawDataMatrix(image,state,*code,*field.bytes,options,diagnostics);
  else if(barcode.symbology==u"BC")drawCode128(image,state,*code,data,options,diagnostics,fonts);
  else if(barcode.symbology==u"B7")drawPdf417(image,state,*code,*field.bytes,options,diagnostics);
  else if(barcode.symbology==u"BD")drawMaxiCode(image,state,*code,*field.bytes,options,diagnostics);
  else if(QStringList{u"BE"_s,u"B8"_s,u"B9"_s,u"BU"_s,u"BA"_s,u"B2"_s,u"BI"_s}.contains(barcode.symbology))drawExtendedLinear(image,state,barcode,*code,data,options,diagnostics,fonts);
  else if(QStringList{u"BF"_s,u"BR"_s,u"BO"_s,u"B0"_s}.contains(barcode.symbology))drawExtendedMatrix(image,state,barcode,*code,field,options,diagnostics);
  else diagnostics.append({Severity::Warning,u"barcode-render-pending"_s,
    u"This barcode parser is available, but its native encoder is not implemented yet."_s,-1,u'^'+barcode.symbology});
  state.pendingBarcode.reset(); state.reverse=false; state.block.reset();
}

} // namespace QtZpl::Rendering
