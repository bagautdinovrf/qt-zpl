#include <QtZpl/qtzpl.hpp>
#include "barcode_encoders.hpp"
#include "pdf417_encoder.hpp"
#include "maxicode_encoder.hpp"

#include <QtCore/QByteArray>
#include <QtCore/QtEndian>
#include <QtGui/QPainter>
#include <QtGui/QPainterPath>
#include <QtGui/QFontDatabase>
#include <QtGui/QTransform>
#include <algorithm>
#include <cmath>
#include <optional>

static void initializeQtZplResources() {
  Q_INIT_RESOURCE(qtzpl_fonts);
}

namespace QtZpl {
namespace {
using namespace Qt::StringLiterals;

struct State {
  QPoint position;
  bool baseline = false;
  bool reverse = false;
  Orientation fieldDirection = Orientation::Normal;
  ScalableFont font;
  std::optional<FieldBlock> block;
  BarcodeDefault barcodeDefaults;
  std::optional<Barcode> pendingBarcode;
  Orientation printOrientation = Orientation::Normal;
};

QColor commandColor(LineColor color, const RenderOptions& options) {
  return color == LineColor::White ? options.background : options.foreground;
}

QString embeddedFontFamily(QStringView resourcePath) {
  const int id=QFontDatabase::addApplicationFont(resourcePath.toString());
  const auto families=QFontDatabase::applicationFontFamilies(id);
  return families.isEmpty()?QString{}:families.front();
}

QString zebraFontFamily() {
  static const QString family=[] {
    initializeQtZplResources();
    return embeddedFontFamily(u":/qtzpl/fonts/font0.ttf");
  }();
  return family;
}

QString fontAFamily() {
  static const QString family=[] {
    initializeQtZplResources();
    return embeddedFontFamily(u":/qtzpl/fonts/font_a.ttf");
  }();
  return family;
}

QString ocrBFontFamily() {
  static const QString family=[] {
    initializeQtZplResources();
    return embeddedFontFamily(u":/qtzpl/fonts/ocr_b.ttf");
  }();
  return family;
}

QFont printerFont(const QString& family,int pixelHeight,int stretch=100) {
  QFont font(family);
  font.setPixelSize(std::max(1,pixelHeight));
  font.setHintingPreference(QFont::PreferFullHinting);
  font.setStyleStrategy(QFont::NoAntialias);
  font.setStretch(std::clamp(stretch,1,400));
  return font;
}

QFont zebraFont(int pixelHeight, int stretch=75) {
  return printerFont(zebraFontFamily(),pixelHeight,stretch);
}

QFont builtInFont(QChar name,int pixelHeight,int requestedWidth=0) {
  if(name.toUpper()==u'A') {
    const int stretch=requestedWidth>0
      ? std::clamp(qRound(100.0*requestedWidth/pixelHeight),1,400)
      : 100;
    return printerFont(fontAFamily(),pixelHeight,stretch);
  }
  constexpr int zebraFont0NaturalStretch=77;
  const int stretch=requestedWidth>0
    ? std::clamp(zebraFont0NaturalStretch*requestedWidth/pixelHeight,1,400)
    : zebraFont0NaturalStretch;
  return zebraFont(pixelHeight,stretch);
}

QFont eanInterpretationFont(int pixelHeight) {
  static const QString family=ocrBFontFamily();
  return printerFont(family,pixelHeight);
}

QSize builtInFontDefaults(QChar font) {
  switch (font.toUpper().unicode()) {
    case u'A': return {5,9};
    case u'B': return {7,11};
    case u'C':
    case u'D': return {10,18};
    case u'E': return {15,28};
    case u'F': return {13,26};
    case u'G': return {40,60};
    case u'H': return {13,21};
    default: return {0,30};
  }
}

QImage rotateImage(const QImage& source, Orientation orientation) {
  QTransform transform;
  switch (orientation) {
    case Orientation::Rotated90: transform.rotate(90); break;
    case Orientation::Inverted: transform.rotate(180); break;
    case Orientation::BottomUp: transform.rotate(270); break;
    default: return source;
  }
  return source.transformed(transform, Qt::FastTransformation);
}

QRect alphaBounds(const QImage& image) {
  QRect bounds;
  for(int y=0;y<image.height();++y) {
    const auto* pixels=reinterpret_cast<const QRgb*>(image.constScanLine(y));
    for(int x=0;x<image.width();++x)
      if(qAlpha(pixels[x])!=0)bounds|=QRect(x,y,1,1);
  }
  return bounds;
}

void drawText(QImage& image, State& state, const QString& text, const RenderOptions& options) {
  if (text.isEmpty()) { state.reverse=false; state.block.reset(); return; }
  const QSize defaults=builtInFontDefaults(state.font.font);
  const int height = state.font.height>0?state.font.height:defaults.height();
  const int width = state.font.width>0?state.font.width:defaults.width();
  const bool fontA=state.font.font.toUpper()==u'A';
  // Zebra Font A is a small bitmap face. Labelary keeps its glyph cell near
  // 20 dots even when ^CF requests a smaller height.
  const int pixelHeight=fontA?std::max(height,20):std::max(1,height-1);
  constexpr int zebraFont0NaturalStretch=77;
  const int requestedStretch=width>0
    ? zebraFont0NaturalStretch*width/height
    : zebraFont0NaturalStretch;
  QFont font=fontA
    ? builtInFont(state.font.font,pixelHeight,state.font.width>0?width:0)
    : zebraFont(pixelHeight,requestedStretch);

  QFontMetrics metrics(font);
  const auto bounds = metrics.boundingRect(text).adjusted(-1,-1,1,1);
  QImage field(std::max(1,bounds.width()), std::max(1,bounds.height()), QImage::Format_ARGB32_Premultiplied);
  field.fill(Qt::transparent);
  QPainter fp(&field);
  fp.setRenderHint(QPainter::TextAntialiasing, false);
  fp.setFont(font);
  fp.setPen(state.reverse ? options.background : options.foreground);
  fp.drawText(-bounds.left(), -bounds.top(), text);
  fp.end();

  const QRect ink=alphaBounds(field);

  const int baselineX = -bounds.left();
  const int baselineY = -bounds.top();
  auto orientation = state.font.orientation == Orientation::Normal ? state.fieldDirection : state.font.orientation;
  field = rotateImage(field, orientation);
  QPoint pos = state.position;
  if(fontA&&!state.baseline&&orientation==Orientation::Normal&&!ink.isEmpty())pos.ry()-=ink.top()+1;
  else if(!state.baseline&&orientation==Orientation::Normal)pos.rx()+=bounds.left();
  if (state.baseline) {
    // ^FT names the baseline origin. Rotate that point with the glyph raster so
    // R/I/B fields remain anchored at ^FT instead of receiving the N-only
    // ascent adjustment after rotation.
    switch (orientation) {
      case Orientation::Normal:
        pos -= QPoint(baselineX, baselineY);
        break;
      case Orientation::Rotated90:
        pos -= QPoint(bounds.height() - 1 - baselineY, baselineX);
        break;
      case Orientation::Inverted:
        pos -= QPoint(bounds.width() - 1 - baselineX, bounds.height() - 1 - baselineY);
        break;
      case Orientation::BottomUp:
        pos -= QPoint(baselineY, bounds.width() - 1 - baselineX);
        break;
    }
  }
  if (state.block && state.block->width > field.width()) {
    if (state.block->justification == Justification::Right) pos.rx() += state.block->width - field.width();
    else if (state.block->justification == Justification::Center) pos.rx() += (state.block->width - field.width()) / 2;
  }
  QPainter painter(&image);
  painter.setCompositionMode(state.reverse ? QPainter::CompositionMode_Difference : QPainter::CompositionMode_SourceOver);
  painter.drawImage(pos, field);
  state.reverse=false; state.block.reset();
}

quint16 z64Crc(QByteArrayView encoded) {
  quint16 crc=0;
  for(const unsigned char byte:encoded){
    crc^=static_cast<quint16>(byte)<<8;
    for(int bit=0;bit<8;++bit)crc=crc&0x8000U?static_cast<quint16>((crc<<1)^0x1021U):static_cast<quint16>(crc<<1);
  }
  return crc;
}

std::optional<QByteArray> decodeZ64(const GraphicField& gf,qsizetype offset,QList<Diagnostic>& diagnostics) {
  constexpr int maximumGraphicBytes=64*1024*1024;
  const auto fail=[&](QString code,QString message)->std::optional<QByteArray>{
    diagnostics.append({Severity::Error,std::move(code),std::move(message),offset,u"^GF"_s});
    return std::nullopt;
  };
  if(gf.totalBytes<=0||gf.totalBytes>maximumGraphicBytes)
    return fail(u"graphic-field-z64-size"_s,u"The Z64 graphic declares an invalid or unsafe decompressed size."_s);

  const auto data=QByteArrayView{gf.data}.trimmed();
  constexpr QByteArrayView prefix{":Z64:"};
  const auto checksumSeparator=data.lastIndexOf(':');
  if(!data.startsWith(prefix)||checksumSeparator<=prefix.size()||data.size()-checksumSeparator-1!=4)
    return fail(u"graphic-field-z64-format"_s,u"The Z64 graphic must use :Z64:<Base64>:<CRC16>."_s);

  const auto encoded=data.sliced(prefix.size(),checksumSeparator-prefix.size());
  bool checksumOk=false;
  const auto expectedCrc=QString::fromLatin1(data.sliced(checksumSeparator+1)).toUShort(&checksumOk,16);
  if(!checksumOk)
    return fail(u"graphic-field-z64-format"_s,u"The Z64 graphic CRC must contain four hexadecimal digits."_s);
  if(z64Crc(encoded)!=expectedCrc)
    return fail(u"graphic-field-z64-crc"_s,u"The Z64 graphic CRC does not match its Base64-encoded data."_s);

  const auto decoded=QByteArray::fromBase64Encoding(encoded.toByteArray(),
    QByteArray::Base64Encoding|QByteArray::AbortOnBase64DecodingErrors);
  if(!decoded)
    return fail(u"graphic-field-z64-base64"_s,u"The Z64 graphic contains invalid Base64 data."_s);

  QByteArray compressedWithSize(4,Qt::Uninitialized);
  qToBigEndian<quint32>(static_cast<quint32>(gf.totalBytes),compressedWithSize.data());
  compressedWithSize.append(decoded.decoded);
  auto uncompressed=qUncompress(compressedWithSize);
  if(uncompressed.size()!=gf.totalBytes)
    return fail(u"graphic-field-z64-zlib"_s,u"The Z64 graphic could not be decompressed to its declared size."_s);
  return uncompressed;
}

void drawGraphicField(QImage& image,const QPoint& pos,const GraphicField& gf,const RenderOptions& options,
                      qsizetype offset,QList<Diagnostic>& diagnostics) {
  if (gf.bytesPerRow <= 0) return;
  int x=0,y=0;
  QPainter painter(&image); painter.setPen(options.foreground);
  const bool z64=gf.compression==u'A'&&QByteArrayView{gf.data}.trimmed().startsWith(":Z64:");
  const auto z64Data=z64?decodeZ64(gf,offset,diagnostics):std::optional<QByteArray>{};
  if(z64&&!z64Data)return;
  if(gf.compression==u'B'||z64){
    const QByteArrayView bytes=z64?QByteArrayView{*z64Data}:QByteArrayView{gf.data};
    for(const unsigned char byte:bytes){
      for(int bit=7;bit>=0;--bit){
        if(byte&(1U<<bit))painter.drawPoint(pos.x()+x,pos.y()+y);
        if(++x>=gf.bytesPerRow*8){x=0;++y;}
      }
    }
    return;
  }
  QByteArray hex = gf.data;
  hex.removeIf([](char c){ return c=='\r'||c=='\n'||c==' '||c=='\t'; });
  for (const char c : hex) {
    int nibble = -1;
    if (c>='0'&&c<='9') nibble=c-'0'; else if(c>='A'&&c<='F') nibble=c-'A'+10; else if(c>='a'&&c<='f') nibble=c-'a'+10;
    if (nibble<0) continue;
    for(int bit=3;bit>=0;--bit) {
      if(nibble&(1<<bit)) painter.drawPoint(pos.x()+x,pos.y()+y);
      if(++x>=gf.bytesPerRow*8){x=0;++y;}
    }
  }
}

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

void drawCode39(QImage& image, State& state, const Barcode& barcode, const QString& data, const RenderOptions& options) {
  const int narrow=std::max(1,state.barcodeDefaults.moduleWidth);
  const int wide=std::max(narrow+1,qRound(narrow*state.barcodeDefaults.wideToNarrowRatio));
  const int height=barcode.parameters.size()>1 && !barcode.parameters[1].isEmpty() ? barcode.parameters[1].toInt() : state.barcodeDefaults.height;
  int x=state.position.x();
  QPainter painter(&image); painter.setBrush(options.foreground); painter.setPen(Qt::NoPen);
  const QString encoded=u'*'+data.toUpper()+u'*';
  for(const auto c:encoded){
    const auto pattern=code39Pattern(c); if(pattern.isEmpty()) continue;
    for(int i=0;i<pattern.size();++i){const int w=pattern[i]==u'w'?wide:narrow;if(i%2==0)painter.drawRect(x,state.position.y(),w,height);x+=w;}x+=narrow;
  }
}

QByteArray decodeDataMatrixData(const QString& text, QChar escape, bool gs1) {
  QByteArray output;
  const auto input=text.toLatin1(); const char marker=escape.toLatin1();
  for(int i=0;i<input.size();++i) {
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

void drawDataMatrix(QImage& image,State& state,const Barcode& barcode,const QString& text,const RenderOptions& options,QList<Diagnostic>& diagnostics) {
  const int module=std::max(1,barcode.parameters.size()>1?barcode.parameters[1].toInt():3);
  const bool gs1=barcode.parameters.size()>5&&barcode.parameters[5].toInt()==1;
  const QChar escape=barcode.parameters.size()>6&&!barcode.parameters[6].isEmpty()?barcode.parameters[6].front():u'~';
  const int columns=barcode.parameters.size()>3?barcode.parameters[3].toInt():0;
  const int rows=barcode.parameters.size()>4?barcode.parameters[4].toInt():0;
  const int requestedSize=columns>0&&columns==rows?columns:0;
  auto matrix=BarcodeEncoders::dataMatrix(decodeDataMatrixData(text,escape,gs1),gs1,requestedSize);
  if(!matrix){diagnostics.append({Severity::Error,u"datamatrix-encode"_s,matrix.error(),-1,u"^BX"_s});return;}
  QImage symbol(matrix->width*module,matrix->height*module,QImage::Format_ARGB32_Premultiplied);symbol.fill(options.background);
  QPainter p(&symbol);p.setPen(Qt::NoPen);p.setBrush(options.foreground);
  for(int y=0;y<matrix->height;++y)for(int x=0;x<matrix->width;++x)if(matrix->at(x,y))p.drawRect(x*module,y*module,module,module);
  p.end(); symbol=rotateImage(symbol,barcode.orientation);
  QPoint position=state.position;if(state.baseline)position.ry()-=symbol.height();
  QPainter target(&image);target.drawImage(position,symbol);
}

void drawQrCode(QImage& image,State& state,const Barcode& barcode,const QString& fieldData,const RenderOptions& options,QList<Diagnostic>& diagnostics) {
  const int model=barcode.parameters.size()>1&&!barcode.parameters[1].isEmpty()
    ? barcode.parameters[1].toInt() : 2;
  if(model!=2){
    diagnostics.append({Severity::Error,u"qrcode-model"_s,
      u"Only QR Model 2 is implemented; no symbol was rendered."_s,-1,u"^BQ"_s});
    return;
  }
  QChar errorCorrection=u'M';
  QString data=fieldData;
  if(data.size()>=3&&data[2]==u','){
    errorCorrection=data[0].toUpper();
    const QChar inputMode=data[1].toUpper();
    data.remove(0,3);
    if(inputMode==u'M'&&!data.isEmpty()){
      const QChar characterMode=data.front().toUpper();
      if(characterMode==u'N'||characterMode==u'A'||characterMode==u'K'){
        data.remove(0,1);
        // Zebra manual alphanumeric mode uses the QR alphanumeric alphabet;
        // lower-case input is normalized to upper case by the printer.
        if(characterMode==u'A'){
          data=data.toUpper();
          data.removeIf([](QChar value){
            return !QStringView{u"0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:"}.contains(value);
          });
        }
      }
      else if(characterMode==u'B'&&data.size()>=5){
        bool ok=false;const int bytes=data.mid(1,4).toInt(&ok);
        if(ok&&bytes<=data.size()-5)data=data.mid(5,bytes);
      }
    }
  }
  const int module=std::clamp(barcode.parameters.size()>2?barcode.parameters[2].toInt():2,1,100);
  auto matrix=BarcodeEncoders::qrCode(data.toUtf8(),errorCorrection);
  if(!matrix){diagnostics.append({Severity::Error,u"qrcode-encode"_s,matrix.error(),-1,u"^BQ"_s});return;}
  QImage symbol(matrix->width*module,matrix->height*module,QImage::Format_ARGB32_Premultiplied);symbol.fill(options.background);
  QPainter painter(&symbol);painter.setPen(Qt::NoPen);painter.setBrush(options.foreground);
  for(int y=0;y<matrix->height;++y)for(int x=0;x<matrix->width;++x)if(matrix->at(x,y))painter.drawRect(x*module,y*module,module,module);
  painter.end();symbol=rotateImage(symbol,barcode.orientation);
  QPoint position=state.position;
  if(state.baseline)position.ry()-=symbol.height();
  else position.ry()+=std::max(0,state.barcodeDefaults.height);
  QPainter target(&image);target.drawImage(position,symbol);
}

void drawEan13(QImage& image,State& state,const Barcode& barcode,const QString& text,const RenderOptions& options,QList<Diagnostic>& diagnostics) {
  QString normalized;auto modules=BarcodeEncoders::ean13(text,&normalized);
  if(!modules){diagnostics.append({Severity::Error,u"ean13-encode"_s,modules.error(),-1,u"^BE"_s});return;}
  const int module=std::max(1,state.barcodeDefaults.moduleWidth);
  int height=state.barcodeDefaults.height>0?state.barcodeDefaults.height:100;
  if(barcode.parameters.size()>1&&!barcode.parameters[1].isEmpty())height=std::max(1,barcode.parameters[1].toInt());
  const bool interpretation=barcode.parameters.size()<=2||barcode.parameters[2].compare(u"N",Qt::CaseInsensitive)!=0;
  const bool interpretationAbove=barcode.parameters.size()>3&&barcode.parameters[3].compare(u"Y",Qt::CaseInsensitive)==0;
  const int guardExtra=std::max(1,5*module-2);
  const int textHeight=interpretation?std::max(12,12*module):0;
  const int leftPad=interpretation?14*module:0;
  const int barsTop=interpretation&&interpretationAbove?textHeight:0;
  QImage symbol(leftPad+95*module,height+guardExtra+textHeight,QImage::Format_ARGB32_Premultiplied);symbol.fill(options.background);
  QPainter p(&symbol);p.setPen(Qt::NoPen);p.setBrush(options.foreground);
  for(int i=0;i<modules->size();++i)if((*modules)[i]){const bool guard=i<3||(i>=45&&i<=49)||i>=92;p.drawRect(leftPad+i*module,barsTop,module,height+(guard?guardExtra:0));}
  if(interpretation){
    // EAN/UPC human-readable interpretation uses OCR-B rather than Zebra Font 0.
    p.setFont(eanInterpretationFont(std::max(12,12*module)));
    p.setPen(options.foreground);
    if(interpretationAbove){
      const int textTop=-(3*module-1);
      p.drawText(QRect(leftPad,textTop,95*module,textHeight),Qt::AlignCenter,normalized);
    }else{
      const int textTop=height-(2*module-1);
      p.drawText(QRect(-module,textTop,leftPad,textHeight),Qt::AlignCenter,normalized.left(1));
      p.drawText(QRect(leftPad+3*module,textTop,42*module,textHeight),Qt::AlignCenter,normalized.mid(1,6));
      p.drawText(QRect(leftPad+50*module,textTop,42*module,textHeight),Qt::AlignCenter,normalized.mid(7,6));
    }
  }
  p.end();
  symbol=rotateImage(symbol,barcode.orientation);
  QPoint position=state.position;
  switch(barcode.orientation){
    case Orientation::Normal:
      position.rx()-=leftPad;
      if(interpretationAbove)position.ry()-=textHeight;
      break;
    case Orientation::Rotated90:
      position.rx()-=textHeight+guardExtra;
      position.ry()-=leftPad;
      break;
    case Orientation::Inverted:
      position.ry()-=textHeight+guardExtra;
      break;
    case Orientation::BottomUp:
      break;
  }
  QPainter target(&image);target.drawImage(position,symbol);
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

void drawCode128(QImage& image,State& state,const Barcode& barcode,const QString& text,const RenderOptions& options,QList<Diagnostic>& diagnostics) {
  if(barcode.parameters.size()>4&&barcode.parameters[4].compare(u"Y",Qt::CaseInsensitive)==0){
    diagnostics.append({Severity::Error,u"code128-ucc-check-digit"_s,
      u"The optional UCC Mod 10 check digit is not implemented; no Code 128 symbol was rendered."_s,-1,u"^BC"_s});
    return;
  }
  const QChar mode=barcode.parameters.size()>5&&!barcode.parameters[5].isEmpty()?barcode.parameters[5].front():u'N';
  auto modules=BarcodeEncoders::code128(text,mode);
  if(!modules){diagnostics.append({Severity::Error,u"code128-encode"_s,modules.error(),-1,u"^BC"_s});return;}
  const int module=std::clamp(state.barcodeDefaults.moduleWidth,1,10);
  int height=state.barcodeDefaults.height>0?state.barcodeDefaults.height:100;
  if(barcode.parameters.size()>1&&!barcode.parameters[1].isEmpty())height=std::max(1,barcode.parameters[1].toInt());
  const bool interpretation=barcode.parameters.size()<=2||barcode.parameters[2].compare(u"N",Qt::CaseInsensitive)!=0;
  const bool interpretationAbove=barcode.parameters.size()>3&&barcode.parameters[3].compare(u"Y",Qt::CaseInsensitive)==0;
  const int gap=interpretation?5:0;
  const int textHeight=interpretation?std::clamp(height/6,18,40):0;
  const int barsTop=interpretationAbove?textHeight+gap:0;
  const int symbolHeight=height+textHeight+gap;
  QImage symbol(modules->size()*module,symbolHeight,QImage::Format_ARGB32_Premultiplied);symbol.fill(options.background);
  QPainter painter(&symbol);painter.setRenderHint(QPainter::Antialiasing,false);painter.setPen(Qt::NoPen);painter.setBrush(options.foreground);
  for(qsizetype i=0;i<modules->size();++i)if((*modules)[i])painter.drawRect(static_cast<int>(i)*module,barsTop,module,height);
  if(interpretation){
    const QString interpretationText=code128Interpretation(text);
    // Zebra's Code 128 interpretation uses Font A with a larger fixed cell
    // than ordinary Font 0 text at the same bar height.
    const int interpretationSize=std::max(textHeight,50);
    const QFont font=builtInFont(u'A',interpretationSize,interpretationSize);
    const QRect bounds=QFontMetrics(font).boundingRect(interpretationText).adjusted(-1,-1,1,1);
    QImage field(std::max(1,bounds.width()),std::max(1,bounds.height()),QImage::Format_ARGB32_Premultiplied);
    field.fill(Qt::transparent);
    QPainter textPainter(&field);
    textPainter.setRenderHint(QPainter::TextAntialiasing,false);
    textPainter.setFont(font);textPainter.setPen(options.foreground);
    textPainter.drawText(-bounds.left(),-bounds.top(),interpretationText);
    textPainter.end();
    const QRect ink=alphaBounds(field);
    const int textTop=interpretationAbove?0:height+gap;
    if(!ink.isEmpty()) {
      const QImage cropped=field.copy(ink);
      painter.drawImage((symbol.width()-cropped.width())/2,textTop,cropped);
    }
  }
  painter.end();symbol=rotateImage(symbol,barcode.orientation);
  QPoint position=state.position;
  if(state.baseline)position.ry()-=symbol.height();
  else if(barcode.orientation==Orientation::Normal)position.ry()-=barsTop;
  QPainter target(&image);target.drawImage(position,symbol);
}

void drawCodabar(QImage& image,State& state,const Barcode& barcode,const QString& text,const RenderOptions& options,QList<Diagnostic>& diagnostics) {
  const int ratio=std::clamp(qRound(state.barcodeDefaults.wideToNarrowRatio),2,3);
  auto modules=BarcodeEncoders::codabar(text,ratio);
  if(!modules){diagnostics.append({Severity::Error,u"codabar-encode"_s,modules.error(),-1,u"^BK"_s});return;}
  const int module=std::clamp(state.barcodeDefaults.moduleWidth,1,10);
  const int height=barcode.parameters.size()>2&&!barcode.parameters[2].isEmpty()?std::max(1,barcode.parameters[2].toInt()):std::max(1,state.barcodeDefaults.height);
  QImage symbol(modules->size()*module,height,QImage::Format_ARGB32_Premultiplied);symbol.fill(options.background);
  QPainter painter(&symbol);painter.setPen(Qt::NoPen);painter.setBrush(options.foreground);
  for(qsizetype i=0;i<modules->size();++i)if((*modules)[i])painter.drawRect(static_cast<int>(i)*module,0,module,height);
  painter.end();symbol=rotateImage(symbol,barcode.orientation);
  QPoint position=state.position;if(state.baseline)position.ry()-=symbol.height();
  QPainter target(&image);target.drawImage(position,symbol);
}

void drawPdf417(QImage& image,State& state,const Barcode& barcode,const QString& text,const RenderOptions& options,QList<Diagnostic>& diagnostics) {
  const int rowHeight=barcode.parameters.size()>1&&!barcode.parameters[1].isEmpty()?std::max(1,barcode.parameters[1].toInt()):std::max(1,state.barcodeDefaults.moduleWidth*3);
  const int security=barcode.parameters.size()>2&&!barcode.parameters[2].isEmpty()?barcode.parameters[2].toInt():0;
  const int columns=barcode.parameters.size()>3&&!barcode.parameters[3].isEmpty()?barcode.parameters[3].toInt():0;
  const int rows=barcode.parameters.size()>4&&!barcode.parameters[4].isEmpty()?barcode.parameters[4].toInt():0;
  const bool truncated=barcode.parameters.size()>5&&barcode.parameters[5].compare(u"Y",Qt::CaseInsensitive)==0;
  if(truncated){diagnostics.append({Severity::Error,u"pdf417-truncated"_s,u"Truncated PDF417 is not implemented; no symbol was rendered."_s,-1,u"^B7"_s});return;}
  auto symbolData=BarcodeEncoders::Pdf417::encode(text.toLatin1(),security,columns,rows);
  if(!symbolData){diagnostics.append({Severity::Error,u"pdf417-encode"_s,symbolData.error(),-1,u"^B7"_s});return;}
  const int module=std::clamp(state.barcodeDefaults.moduleWidth,1,10);
  const auto& matrix=symbolData->matrix;
  QImage symbol(matrix.width*module,matrix.height*rowHeight,QImage::Format_ARGB32_Premultiplied);symbol.fill(options.background);
  QPainter painter(&symbol);painter.setPen(Qt::NoPen);painter.setBrush(options.foreground);
  for(int y=0;y<matrix.height;++y)for(int x=0;x<matrix.width;++x)if(matrix.at(x,y))painter.drawRect(x*module,y*rowHeight,module,rowHeight);
  painter.end();symbol=rotateImage(symbol,barcode.orientation);
  QPoint position=state.position;if(state.baseline)position.ry()-=symbol.height();
  QPainter target(&image);target.drawImage(position,symbol);
}

// MaxiCode raster geometry adapted from libzint backend/raster.c.
// Copyright (C) 2009-2026 Robin Stuart and contributors, BSD-3-Clause.
// The integer line/circle primitives are based on Alois Zingl's MIT-licensed
// Bresenham algorithms. See third_party/zint-raster/LICENSE.
void setRasterPixel(QImage& image,int x,int y,QRgb colour) {
  if(image.rect().contains(x,y))image.setPixel(x,y,colour);
}

void plotRasterLine(QImage& image,int x0,int y0,int x1,int y1,QRgb colour) {
  const int dx=std::abs(x1-x0),sx=x0<x1?1:-1;
  const int dy=-std::abs(y1-y0),sy=y0<y1?1:-1;
  int error=dx+dy;
  for(;;){
    setRasterPixel(image,x0,y0,colour);
    if(x0==x1&&y0==y1)break;
    const int twiceError=2*error;
    if(twiceError>=dy){error+=dy;x0+=sx;}
    if(twiceError<=dx){error+=dx;y0+=sy;}
  }
}

QImage maxiCodeHexagon(int width,int height,int left,int top,int right,int bottom,QRgb ink) {
  QImage hexagon(width,height,QImage::Format_ARGB32_Premultiplied);hexagon.fill(Qt::transparent);
  const int innerWidth=width-left-right,innerHeight=height-top-bottom;
  const int radiusX=innerWidth/2,radiusY=innerHeight/2;
  int startY=top+(radiusY+1)/2,endX=left+radiusX;
  if(radiusX>2&&(radiusX<10||!(innerWidth&1)))--endX;
  plotRasterLine(hexagon,left,startY,endX,top,ink);
  bool foundPreviousLine=false;
  for(int y=top;y<top+radiusY+(innerHeight&1);++y){
    int first=-1;
    for(int x=left;x<left+radiusX+(innerWidth&1);++x){
      if(first!=-1){setRasterPixel(hexagon,x,y,ink);foundPreviousLine=true;}
      else if(qAlpha(hexagon.pixel(x,y))!=0)first=x+1;
    }
    if(foundPreviousLine&&first==-1)for(int x=left;x<left+radiusX+(innerWidth&1);++x)setRasterPixel(hexagon,x,y,ink);
  }
  for(int y=top;y<top+radiusY+(innerHeight&1);++y)
    for(int x=left;x<left+radiusX+(innerWidth&1);++x)if(qAlpha(hexagon.pixel(x,y))!=0)
      setRasterPixel(hexagon,width-right-(x-left+1),y,ink);
  for(int y=top;y<top+radiusY+(innerHeight&1);++y)
    for(int x=left;x<width;++x)if(qAlpha(hexagon.pixel(x,y))!=0)
      setRasterPixel(hexagon,x,height-bottom-(y-top+1),ink);
  return hexagon;
}

void fillMidpointCircleLines(QImage& image,int x0,int y0,int x,int y,QRgb colour) {
  for(int i=x0-x;i<=x0+x;++i){setRasterPixel(image,i,y0+y,colour);setRasterPixel(image,i,y0-y,colour);}
  for(int i=x0-y;i<=x0+y;++i){setRasterPixel(image,i,y0+x,colour);setRasterPixel(image,i,y0-x,colour);}
}

void fillMidpointCircle(QImage& image,int x0,int y0,int radius,QRgb colour) {
  int x=-radius,y=0,error=2-2*radius;
  do{
    fillMidpointCircleLines(image,x0,y0,x,y,colour);
    radius=error;
    if(radius<=y)error+=++y*2+1;
    if(radius>x||error>y)error+=++x*2+1;
  }while(x<0);
  fillMidpointCircleLines(image,x0,y0,x,y,colour);
}

QImage rasterizeMaxiCode(const BarcodeEncoders::Matrix& grid,const RenderOptions& options) {
  // Work at ten times the printer-dot resolution. This preserves Zint's
  // integer construction while allowing the 203-DPI MaxiCode pitch (6.7 dots
  // horizontally, 5.8 vertically) to be sampled deterministically.
  constexpr int supersample=10;
  constexpr double scaler=67.0;
  const int hexWidth=qRound(scaler),hexHeight=qRound(scaler*1.1547);
  const int left=static_cast<int>(std::ceil(hexWidth*0.05));
  const int top=static_cast<int>(std::ceil(hexHeight*0.05));
  const int right=qRound((hexWidth-left)*0.05),bottom=qRound((hexHeight-top)*0.05);
  const int rowStep=qRound(0.866*hexWidth);
  const int width=30*hexWidth-left-right,height=32*rowStep+hexHeight-top-bottom;
  QImage symbol(width,height,QImage::Format_ARGB32_Premultiplied);symbol.fill(options.background);
  const QRgb ink=options.foreground.rgba(),paper=options.background.rgba();
  const auto hexagon=maxiCodeHexagon(hexWidth,hexHeight,left,top,right,bottom,ink);
  QPainter painter(&symbol);painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
  for(int row=0;row<33;++row){
    const int odd=row&1,y=row*rowStep-top,xOffset=(odd?hexWidth/2:0)-left;
    for(int column=0;column<30-odd;++column)if(grid.at(column,row))painter.drawImage(column*hexWidth+xOffset,y,hexagon);
  }
  painter.end();
  // Sampling a 10x raster places this continuous centre in the preceding
  // output pixel; bias by one printer dot to retain the ISO 14.5X centre.
  const int centerX=static_cast<int>(14.5*hexWidth-left)+supersample;
  const int centerY=static_cast<int>(std::ceil(height/2.0));
  const int innerRadius=static_cast<int>(std::ceil(hexHeight/2.0));
  int increment=((hexWidth*9-innerRadius)/5)/2;if(increment>right)increment-=right;
  fillMidpointCircle(symbol,centerX,centerY,innerRadius+increment*5,ink);
  fillMidpointCircle(symbol,centerX,centerY,innerRadius+increment*4,paper);
  fillMidpointCircle(symbol,centerX,centerY,innerRadius+increment*3,ink);
  fillMidpointCircle(symbol,centerX,centerY,innerRadius+increment*2,paper);
  fillMidpointCircle(symbol,centerX,centerY,innerRadius+increment,ink);
  fillMidpointCircle(symbol,centerX,centerY,innerRadius,paper);
  QImage sampled((width+supersample-1)/supersample,(height+supersample-1)/supersample,
    QImage::Format_ARGB32_Premultiplied);sampled.fill(options.background);
  for(int y=0;y<sampled.height();++y)for(int x=0;x<sampled.width();++x)
    sampled.setPixel(x,y,symbol.pixel(std::min(width-1,x*supersample+supersample/2),
      std::min(height-1,y*supersample+supersample/2)));
  return sampled;
}

void drawMaxiCode(QImage& image,State& state,const Barcode& barcode,const QString& text,const RenderOptions& options,QList<Diagnostic>& diagnostics) {
  const int mode=barcode.parameters.isEmpty()?2:barcode.parameters[0].toInt();
  if(mode!=2){diagnostics.append({Severity::Error,u"maxicode-mode"_s,u"Only MaxiCode mode 2 is implemented; no symbol was rendered."_s,-1,u"^BD"_s});return;}
  auto encoded=BarcodeEncoders::MaxiCode::encodeMode2(text.toLatin1());
  if(!encoded){diagnostics.append({Severity::Error,u"maxicode-encode"_s,encoded.error(),-1,u"^BD"_s});return;}
  QImage symbol=rotateImage(rasterizeMaxiCode(encoded->grid,options),barcode.orientation);
  QPoint position=state.position;if(state.baseline)position.ry()-=symbol.height();else position.ry()+=1;
  QPainter target(&image);target.drawImage(position,symbol);
}

void drawBarcode(QImage& image, State& state, const Barcode& barcode, const QString& data, const RenderOptions& options, QList<Diagnostic>& diagnostics) {
  if (barcode.symbology == u"B3") drawCode39(image,state,barcode,data,options);
  else if(barcode.symbology==u"BQ")drawQrCode(image,state,barcode,data,options,diagnostics);
  else if(barcode.symbology==u"BX")drawDataMatrix(image,state,barcode,data,options,diagnostics);
  else if(barcode.symbology==u"BE")drawEan13(image,state,barcode,data,options,diagnostics);
  else if(barcode.symbology==u"BC")drawCode128(image,state,barcode,data,options,diagnostics);
  else if(barcode.symbology==u"BK")drawCodabar(image,state,barcode,data,options,diagnostics);
  else if(barcode.symbology==u"B7")drawPdf417(image,state,barcode,data,options,diagnostics);
  else if(barcode.symbology==u"BD")drawMaxiCode(image,state,barcode,data,options,diagnostics);
  else diagnostics.append({Severity::Warning,u"barcode-render-pending"_s,
    u"This barcode parser is available, but its native encoder is not implemented yet."_s,-1,u'^'+barcode.symbology});
  state.pendingBarcode.reset(); state.reverse=false; state.block.reset();
}

std::expected<QImage, RenderError> renderLabel(const Label& label, int index, const RenderOptions& options, QList<Diagnostic>& diagnostics) {
  const int width=options.width>0?options.width:(label.width()>0?label.width():812);
  const int height=options.height>0?options.height:(label.height()>0?label.height():1218);
  if(width<=0||height<=0||width>100000||height>100000) return std::unexpected(RenderError{u"invalid-size"_s,u"Label dimensions are invalid or unsafe."_s,index});
  QImage image(width,height,QImage::Format_ARGB32_Premultiplied); image.fill(options.background);
  State state; state.font=ScalableFont{};
  const QPoint home=options.ignoreLabelHome?QPoint{}:QPoint{label.homeX(),label.homeY()};

  const auto prepareGraphicPainter=[&](QPainter& painter,LineColor color,int thickness){
    if(state.reverse){painter.setCompositionMode(QPainter::CompositionMode_Difference);painter.setPen(QPen(Qt::white,thickness));painter.setBrush(Qt::white);}
    else{const auto drawColor=commandColor(color,options);painter.setPen(QPen(drawColor,thickness));painter.setBrush(drawColor);}
  };

  for(const auto& command:label.commands()) {
    std::visit([&](const auto& value){
      using T=std::decay_t<decltype(value)>;
      if constexpr(std::is_same_v<T,FieldOrigin>){state.position=QPoint(value.x,value.y)+home;state.baseline=false;}
      else if constexpr(std::is_same_v<T,FieldTypeset>){state.position=QPoint(value.x,value.y)+home;state.baseline=true;}
      else if constexpr(std::is_same_v<T,ScalableFont>) state.font=value;
      else if constexpr(std::is_same_v<T,ChangeFont>){state.font.font=value.font;state.font.height=value.height;state.font.width=value.width;}
      else if constexpr(std::is_same_v<T,FieldDirection>) state.fieldDirection=value.orientation;
      else if constexpr(std::is_same_v<T,FieldReverse>) state.reverse=true;
      else if constexpr(std::is_same_v<T,FieldBlock>) state.block=value;
      else if constexpr(std::is_same_v<T,BarcodeDefault>) state.barcodeDefaults=value;
      else if constexpr(std::is_same_v<T,PrintOrientation>) state.printOrientation=value.orientation;
      else if constexpr(std::is_same_v<T,Barcode>) state.pendingBarcode=value;
      else if constexpr(std::is_same_v<T,FieldData>){if(state.pendingBarcode)drawBarcode(image,state,*state.pendingBarcode,value.data,options,diagnostics);else drawText(image,state,value.data,options);}
      else if constexpr(std::is_same_v<T,GraphicBox>){
        const auto fillRectangle=QRect(state.position,QSize(value.width,value.height));
        const bool filled=value.thickness*2>=std::min(value.width,value.height);
        QPainterPath path;path.setFillRule(Qt::OddEvenFill);path.addRect(QRectF(fillRectangle));
        if(!filled){const int inset=std::min({value.thickness,value.width/2,value.height/2});path.addRect(QRectF(fillRectangle.adjusted(inset,inset,-inset,-inset)));}
        QPainter p(&image);p.setRenderHint(QPainter::Antialiasing,false);p.setPen(Qt::NoPen);p.setBrush(state.reverse?Qt::white:commandColor(value.color,options));
        if(state.reverse)p.setCompositionMode(QPainter::CompositionMode_Difference);p.drawPath(path);
        state.reverse=false;
      }
      else if constexpr(std::is_same_v<T,GraphicCircle>||std::is_same_v<T,GraphicEllipse>){
        const int width=[](const auto& graphic){if constexpr(std::is_same_v<T,GraphicCircle>)return graphic.diameter;else return graphic.width;}(value);
        const int height=[](const auto& graphic){if constexpr(std::is_same_v<T,GraphicCircle>)return graphic.diameter;else return graphic.height;}(value);
        const QRectF outer(state.position.x(),state.position.y()+1,width,std::max(0,height-1));const int inset=std::min({value.thickness,width/2,height/2});
        QPainterPath path;path.setFillRule(Qt::OddEvenFill);path.addEllipse(outer);
        if(value.thickness*2<std::min(width,height))path.addEllipse(outer.adjusted(inset,inset,-inset,-inset));
        QPainter p(&image);p.setRenderHint(QPainter::Antialiasing,false);p.setPen(Qt::NoPen);p.setBrush(state.reverse?Qt::white:commandColor(value.color,options));
        if(state.reverse)p.setCompositionMode(QPainter::CompositionMode_Difference);p.drawPath(path);state.reverse=false;
      }
      else if constexpr(std::is_same_v<T,GraphicDiagonal>){
        QPainter p(&image);p.setPen(Qt::NoPen);p.setBrush(state.reverse?Qt::white:commandColor(value.color,options));
        if(state.reverse)p.setCompositionMode(QPainter::CompositionMode_Difference);
        // Zebra rasterizes ^GD as a constant number of dots on every scan
        // line. R descends from the upper-right; L ascends from upper-left.
        for(int y=0;y<value.height;++y){
          const int ascending=y*value.width/value.height;
          const int x=value.orientation==u'L'?ascending:value.width-ascending;
          p.drawRect(state.position.x()+x,state.position.y()+y,value.thickness,1);
        }
        state.reverse=false;
      }
      else if constexpr(std::is_same_v<T,GraphicField>) drawGraphicField(image,state.position,value,options,command.offset,diagnostics);
    },command.payload);
  }
  image=rotateImage(image,state.printOrientation);
  return image;
}

} // namespace

std::expected<RenderResult, RenderError> render(const Document& document, const RenderOptions& options) {
  if(options.dpi!=203&&options.dpi!=300&&options.dpi!=600) return std::unexpected(RenderError{u"invalid-dpi"_s,u"DPI must be 203, 300, or 600."_s,-1});
  RenderResult result; result.diagnostics=document.diagnostics(); result.labels.reserve(document.labels().size());
  for(int i=0;i<document.labels().size();++i){auto image=renderLabel(document.labels()[i],i,options,result.diagnostics);if(!image)return std::unexpected(image.error());result.labels.append(std::move(*image));}
  return result;
}

std::expected<RenderResult, Error> render(QStringView zpl, const ParseOptions& parseOptions, const RenderOptions& renderOptions) {
  auto document=parse(zpl,parseOptions);
  if(!document)return std::unexpected(Error{Error::Stage::Parse,document.error().code,document.error().message,document.error().offset,-1});
  auto result=render(*document,renderOptions);
  if(!result)return std::unexpected(Error{Error::Stage::Render,result.error().code,result.error().message,-1,result.error().labelIndex});
  return *result;
}

} // namespace QtZpl
