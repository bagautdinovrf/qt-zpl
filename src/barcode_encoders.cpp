#include "barcode_encoders.hpp"
#include "qrcodegen.hpp"

#include <array>
#include <algorithm>
#include <limits>
#include <string_view>

namespace QtZpl::BarcodeEncoders {
namespace {

struct DmSize {
  int rows, columns, regionHorizontal, regionVertical, ecc, blocks;
  int regionRows() const { return (rows - regionVertical * 2) / regionVertical; }
  int regionColumns() const { return (columns - regionHorizontal * 2) / regionHorizontal; }
  int matrixRows() const { return regionRows() * regionVertical; }
  int matrixColumns() const { return regionColumns() * regionHorizontal; }
  int dataWords() const { return matrixRows() * matrixColumns() / 8 - ecc; }
  int blockDataWords(int block) const { return rows == 144 ? (block < 8 ? 156 : 155) : dataWords() / blocks; }
};

constexpr std::array<DmSize,24> sizes{{
  DmSize{10,10,1,1,5,1}, {12,12,1,1,7,1}, {14,14,1,1,10,1}, {16,16,1,1,12,1},
  {18,18,1,1,14,1}, {20,20,1,1,18,1}, {22,22,1,1,20,1}, {24,24,1,1,24,1},
  {26,26,1,1,28,1}, {32,32,2,2,36,1}, {36,36,2,2,42,1}, {40,40,2,2,48,1},
  {44,44,2,2,56,1}, {48,48,2,2,68,1}, {52,52,2,2,84,2}, {64,64,4,4,112,2},
  {72,72,4,4,144,4}, {80,80,4,4,192,4}, {88,88,4,4,224,4}, {96,96,4,4,272,4},
  {104,104,4,4,336,6}, {120,120,6,6,408,6}, {132,132,6,6,496,8}, {144,144,6,6,620,10}
}};

class Galois final {
public:
  Galois() {
    int value = 1;
    for (int i=0;i<255;++i) {
      exp_[i]=value; log_[value]=i; value <<= 1;
      if (value & 0x100) value ^= 0x12d;
    }
    for (int i=255;i<512;++i) exp_[i]=exp_[i-255];
  }
  int multiply(int a,int b) const { return a==0||b==0 ? 0 : exp_[log_[a]+log_[b]]; }
  int power(int n) const { return exp_[n%255]; }
private:
  std::array<int,512> exp_{};
  std::array<int,256> log_{};
};

QByteArray rsEncode(const QByteArray& input, int eccCount) {
  static const Galois gf;
  QVector<int> generator{1};
  for (int degree=1;degree<=eccCount;++degree) {
    QVector<int> next(generator.size()+1);
    for (int i=0;i<generator.size();++i) {
      next[i] ^= generator[i];
      next[i+1] ^= gf.multiply(generator[i],gf.power(degree));
    }
    generator=std::move(next);
  }
  QVector<int> message(input.size()+eccCount);
  for(int i=0;i<input.size();++i) message[i]=static_cast<unsigned char>(input[i]);
  for(int i=0;i<input.size();++i) {
    const int coefficient=message[i]; if(coefficient==0) continue;
    for(int j=0;j<generator.size();++j) message[i+j]^=gf.multiply(generator[j],coefficient);
  }
  QByteArray result(eccCount,Qt::Uninitialized);
  for(int i=0;i<eccCount;++i) result[i]=static_cast<char>(message[input.size()+i]);
  return result;
}

QByteArray encodeAscii(const QByteArray& content, bool gs1) {
  QByteArray result;
  if(gs1) result.append(char(232));
  for(int i=0;i<content.size();) {
    const auto c=static_cast<unsigned char>(content[i++]);
    if(c>='0'&&c<='9'&&i<content.size()&&content[i]>='0'&&content[i]<='9') {
      result.append(char(130+(c-'0')*10+(content[i++]-'0')));
    } else if(c>127) { result.append(char(235)); result.append(char(c-127)); }
    else result.append(char(c+1));
  }
  return result;
}

void pad(QByteArray& data,int count) {
  if(data.size()<count) data.append(char(129));
  while(data.size()<count) {
    const int random=((149*(data.size()+1))%253)+1;
    int value=129+random; if(value>254)value-=254; data.append(char(value));
  }
}

QByteArray addEcc(QByteArray data,const DmSize& size) {
  const int dataSize=data.size(); data.resize(dataSize+size.ecc);
  const int eccPerBlock=size.ecc/size.blocks;
  for(int block=0;block<size.blocks;++block) {
    QByteArray part; part.reserve(size.blockDataWords(block));
    for(int i=block;i<dataSize;i+=size.blocks) part.append(data[i]);
    const auto ecc=rsEncode(part,eccPerBlock);
    for(int i=0;i<eccPerBlock;++i) data[dataSize+block+i*size.blocks]=ecc[i];
  }
  return data;
}

class Layout final {
public:
  explicit Layout(const DmSize& s):size(s),bits(s.matrixRows()*s.matrixColumns()),used(bits.size()){}
  void set(int row,int col,unsigned char value,int bit) {
    if(row<0){row+=size.matrixRows();col+=4-((size.matrixRows()+4)%8);}
    if(col<0){col+=size.matrixColumns();row+=4-((size.matrixColumns()+4)%8);}
    const int index=col+row*size.matrixColumns(); used[index]=true; bits[index]=((value>>(7-bit))&1)!=0;
  }
  bool occupied(int row,int col) const{return used[col+row*size.matrixColumns()];}
  void utah(int row,int col,unsigned char v){set(row-2,col-2,v,0);set(row-2,col-1,v,1);set(row-1,col-2,v,2);set(row-1,col-1,v,3);set(row-1,col,v,4);set(row,col-2,v,5);set(row,col-1,v,6);set(row,col,v,7);}
  void corner1(unsigned char v){set(size.matrixRows()-1,0,v,0);set(size.matrixRows()-1,1,v,1);set(size.matrixRows()-1,2,v,2);set(0,size.matrixColumns()-2,v,3);set(0,size.matrixColumns()-1,v,4);set(1,size.matrixColumns()-1,v,5);set(2,size.matrixColumns()-1,v,6);set(3,size.matrixColumns()-1,v,7);}
  void corner2(unsigned char v){set(size.matrixRows()-3,0,v,0);set(size.matrixRows()-2,0,v,1);set(size.matrixRows()-1,0,v,2);set(0,size.matrixColumns()-4,v,3);set(0,size.matrixColumns()-3,v,4);set(0,size.matrixColumns()-2,v,5);set(0,size.matrixColumns()-1,v,6);set(1,size.matrixColumns()-1,v,7);}
  void corner3(unsigned char v){set(size.matrixRows()-3,0,v,0);set(size.matrixRows()-2,0,v,1);set(size.matrixRows()-1,0,v,2);set(0,size.matrixColumns()-2,v,3);set(0,size.matrixColumns()-1,v,4);set(1,size.matrixColumns()-1,v,5);set(2,size.matrixColumns()-1,v,6);set(3,size.matrixColumns()-1,v,7);}
  void corner4(unsigned char v){set(size.matrixRows()-1,0,v,0);set(size.matrixRows()-1,size.matrixColumns()-1,v,1);set(0,size.matrixColumns()-3,v,2);set(0,size.matrixColumns()-2,v,3);set(0,size.matrixColumns()-1,v,4);set(1,size.matrixColumns()-3,v,5);set(1,size.matrixColumns()-2,v,6);set(1,size.matrixColumns()-1,v,7);}
  void place(const QByteArray& data) {
    int index=0,row=4,col=0;
    while(row<size.matrixRows()||col<size.matrixColumns()) {
      if(row==size.matrixRows()&&col==0)corner1(static_cast<unsigned char>(data[index++]));
      if(row==size.matrixRows()-2&&col==0&&size.matrixColumns()%4!=0)corner2(static_cast<unsigned char>(data[index++]));
      if(row==size.matrixRows()-2&&col==0&&size.matrixColumns()%8==4)corner3(static_cast<unsigned char>(data[index++]));
      if(row==size.matrixRows()+4&&col==2&&size.matrixColumns()%8==0)corner4(static_cast<unsigned char>(data[index++]));
      do{if(row<size.matrixRows()&&col>=0&&!occupied(row,col))utah(row,col,static_cast<unsigned char>(data[index++]));row-=2;col+=2;}while(row>=0&&col<size.matrixColumns());
      row+=1;col+=3;
      do{if(row>=0&&col<size.matrixColumns()&&!occupied(row,col))utah(row,col,static_cast<unsigned char>(data[index++]));row+=2;col-=2;}while(row<size.matrixRows()&&col>=0);
      row+=3;col+=1;
    }
    if(!occupied(size.matrixRows()-1,size.matrixColumns()-1)){set(size.matrixRows()-1,size.matrixColumns()-1,255,0);set(size.matrixRows()-2,size.matrixColumns()-2,255,0);}
  }
  Matrix merge() const {
    Matrix out{size.columns,size.rows,QVector<bool>(size.columns*size.rows)};
    auto mark=[&](int x,int y,bool value=true){out.modules[y*out.width+x]=value;};
    for(int y=0;y<size.rows;y+=size.regionRows()+2)for(int x=0;x<size.columns;x+=2)mark(x,y);
    for(int y=size.regionRows()+1;y<size.rows;y+=size.regionRows()+2)for(int x=0;x<size.columns;++x)mark(x,y);
    for(int x=size.regionColumns()+1;x<size.columns;x+=size.regionColumns()+2)for(int y=1;y<size.rows;y+=2)mark(x,y);
    for(int x=0;x<size.columns;x+=size.regionColumns()+2)for(int y=0;y<size.rows;++y)mark(x,y);
    for(int hr=0;hr<size.regionHorizontal;++hr)for(int vr=0;vr<size.regionVertical;++vr)
      for(int x=0;x<size.regionColumns();++x)for(int y=0;y<size.regionRows();++y){
        const int mx=size.regionColumns()*hr+x,my=size.regionRows()*vr+y;
        mark((size.regionColumns()+2)*hr+x+1,(size.regionRows()+2)*vr+y+1,bits[mx+my*size.matrixColumns()]);
      }
    return out;
  }
private:
  const DmSize& size; QVector<bool> bits,used;
};

int eanCheck(QStringView first12){int sum=0;for(int i=0;i<12;++i)sum+=(first12[i].digitValue())*(i%2?3:1);return(10-sum%10)%10;}

constexpr int codeA=101,codeB=100,codeC=99,fnc1=102,startA=103,startB=104,startC=105;

bool code128Digit(QChar c) { return c>=u'0'&&c<=u'9'; }

std::expected<int,QString> code128DataValue(QChar c,char subset) {
  const int value=c.unicode();
  if(subset=='A'&&value<=95)return value<32?value+64:value-32;
  if(subset=='B'&&value>=32&&value<=127)return value-32;
  return std::unexpected(QStringLiteral("Character U+%1 cannot be encoded in Code 128 subset %2")
    .arg(value,4,16,QChar(u'0')).arg(QChar::fromLatin1(subset)));
}

void appendCode128Checksum(QVector<int>& words) {
  int checksum=words.front();
  for(qsizetype i=1;i<words.size();++i)checksum+=static_cast<int>(i)*words[i];
  words.append(checksum%103);
}

std::expected<QVector<int>,QString> code128Explicit(QStringView data) {
  QVector<int> words;
  char subset='B';
  bool shift=false;
  qsizetype i=0;
  if(data.size()>=2&&data.front()==u'>'){
    const auto start=data[1];
    if(start==u'9'){words.append(startA);subset='A';i=2;}
    else if(start==u':'){words.append(startB);subset='B';i=2;}
    else if(start==u';'){words.append(startC);subset='C';i=2;}
  }
  if(words.isEmpty())words.append(startB);
  while(i<data.size()){
    if(data[i]==u'>'&&i+1<data.size()){
      const auto invocation=data[i+1];
      int value=-1;
      if(invocation==u'<')value=62;
      else if(invocation==u'0')value=30;
      else if(invocation==u'=')value=94;
      else if(invocation>=u'1'&&invocation<=u'8')value=94+invocation.digitValue();
      else if(invocation==u'9'||invocation==u':'||invocation==u';')
        return std::unexpected(QStringLiteral("Code 128 start invocation is only valid at the beginning"));
      if(value>=0){
        words.append(value);i+=2;
        if(value==codeC)subset='C';
        else if(value==codeB&&subset!='B')subset='B';
        else if(value==codeA&&subset!='A')subset='A';
        else if(value==98)shift=true;
        continue;
      }
    }
    if(subset=='C'){
      if(i+1>=data.size()||!code128Digit(data[i])||!code128Digit(data[i+1]))
        return std::unexpected(QStringLiteral("Code 128 subset C requires digit pairs"));
      words.append(data[i].digitValue()*10+data[i+1].digitValue());i+=2;continue;
    }
    const char effective=shift?(subset=='A'?'B':'A'):subset;
    auto value=code128DataValue(data[i],effective);
    if(!value)return std::unexpected(value.error());
    words.append(*value);++i;shift=false;
  }
  if(shift)return std::unexpected(QStringLiteral("Code 128 SHIFT must be followed by a character"));
  appendCode128Checksum(words);return words;
}

// A private sentinel distinguishes a GS1 FNC1 invocation from an ordinary ASCII
// GS byte. The latter is data in subset A, not the FNC1 symbol character.
constexpr QChar code128Fnc1Token{0x100};

std::expected<QVector<int>,QString> code128Auto(QStringView data,bool gs1=false) {
  if(data.isEmpty())return std::unexpected(QStringLiteral("Code 128 requires field data"));
  QVector<int> words;
  char subset='B';
  qsizetype i=0;
  auto digitRun=[&](qsizetype from){qsizetype end=from;while(end<data.size()&&code128Digit(data[end]))++end;return end-from;};
  const auto initialDigits=digitRun(0);
  if(initialDigits>=4){words.append(startC);subset='C';}
  else words.append(startB);
  if(gs1)words.append(fnc1);
  while(i<data.size()){
    if(gs1&&data[i]==code128Fnc1Token){words.append(fnc1);++i;continue;}
    const auto run=digitRun(i);
    if(subset=='B'&&run>=4){
      if(run%2!=0){auto value=code128DataValue(data[i++],'B');if(!value)return std::unexpected(value.error());words.append(*value);}
      words.append(codeC);subset='C';continue;
    }
    if(subset=='C'){
      if(run>=2){words.append(data[i].digitValue()*10+data[i+1].digitValue());i+=2;continue;}
      words.append(codeB);subset='B';continue;
    }
    auto value=code128DataValue(data[i++],'B');
    if(!value)return std::unexpected(value.error());
    words.append(*value);
  }
  appendCode128Checksum(words);return words;
}

std::expected<QVector<int>,QString> code128Gs1(QStringView data) {
  QString normalized;normalized.reserve(data.size());
  for(qsizetype i=0;i<data.size();++i){
    const auto character=data[i];
    if(character==u'('||character==u')'||character==u' ')continue;
    if(character==u'>'&&i+1<data.size()){
      const auto invocation=data[i+1];
      if(invocation==u'8'){normalized.append(code128Fnc1Token);++i;continue;}
      if(invocation==u'0'){normalized.append(u'>');++i;continue;}
      // Mode D selects the appropriate subset itself. Explicit leading start
      // codes are accepted for existing Zebra GS1 formats, but do not disable
      // automatic packing of later application identifiers.
      if(normalized.isEmpty()&&(invocation==u'9'||invocation==u':'||invocation==u';')){++i;continue;}
    }
    if(character.unicode()>127)
      return std::unexpected(QStringLiteral("GS1-128 requires ASCII field data"));
    normalized.append(character);
  }
  // A leading >8 is often included in legacy formats; mode D already supplies
  // that FNC1. Preserve subsequent separators, including consecutive ones.
  if(normalized.startsWith(code128Fnc1Token))normalized.remove(0,1);
  return code128Auto(normalized,true);
}

constexpr std::array<const char*,107> code128Patterns{{
  "212222","222122","222221","121223","121322","131222","122213","122312","132212","221213",
  "221312","231212","112232","122132","122231","113222","123122","123221","223211","221132",
  "221231","213212","223112","312131","311222","321122","321221","312212","322112","322211",
  "212123","212321","232121","111323","131123","131321","112313","132113","132311","211313",
  "231113","231311","112133","112331","132131","113123","113321","133121","313121","211331",
  "231131","213113","213311","213131","311123","311321","331121","312113","312311","332111",
  "314111","221411","431111","111224","111422","121124","121421","141122","141221","112214",
  "112412","122114","122411","142112","142211","241211","221114","413111","241112","134111",
  "111242","121142","121241","114212","124112","124211","411212","421112","421211","212141",
  "214121","412121","111143","111341","131141","114113","114311","411113","411311","113141",
  "114131","311141","411131","211412","211214","211232","2331112"
}};

Matrix qrMatrix(const qrcodegen::QrCode& qr) {
  Matrix matrix{qr.getSize(),qr.getSize(),QVector<bool>(qr.getSize()*qr.getSize())};
  for(int y=0;y<matrix.height;++y)for(int x=0;x<matrix.width;++x)matrix.modules[y*matrix.width+x]=qr.getModule(x,y);
  return matrix;
}

int zebraQrPenalty(const Matrix& matrix) {
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

} // namespace

std::expected<Matrix,QString> qrCode(const QByteArray& data,QChar errorCorrection,int mask) {
  using qrcodegen::QrCode;
  QrCode::Ecc ecc=QrCode::Ecc::MEDIUM;
  switch(errorCorrection.toUpper().unicode()){
    case 'L':ecc=QrCode::Ecc::LOW;break;
    case 'Q':ecc=QrCode::Ecc::QUARTILE;break;
    case 'H':ecc=QrCode::Ecc::HIGH;break;
    default:break;
  }
  try{
    const auto segments=qrcodegen::QrSegment::makeSegments(data.constData());
    const auto encode=[&](int selectedMask){return QrCode::encodeSegments(segments,ecc,1,40,selectedMask,true);};
    if(mask>=0)return qrMatrix(encode(mask));
    Matrix best;int bestPenalty=std::numeric_limits<int>::max();
    for(int candidate=0;candidate<8;++candidate){
      auto matrix=qrMatrix(encode(candidate));
      const int penalty=zebraQrPenalty(matrix);
      if(penalty<bestPenalty){bestPenalty=penalty;best=std::move(matrix);}
    }
    return best;
  }catch(const std::exception& error){
    return std::unexpected(QString::fromUtf8(error.what()));
  }
}

std::expected<Matrix,QString> dataMatrix(QByteArray data,bool gs1,int requestedSize) {
  auto words=Detail::dataMatrixCodewords(data,gs1); const DmSize* selected=nullptr;
  if(requestedSize>0){
    for(const auto& size:sizes)if(size.rows==requestedSize&&size.columns==requestedSize){selected=&size;break;}
    if(!selected)return std::unexpected(QStringLiteral("Requested DataMatrix size is unsupported"));
    if(selected->dataWords()<words.size())return std::unexpected(QStringLiteral("Data does not fit the requested DataMatrix size"));
  }else for(const auto& size:sizes)if(size.dataWords()>=words.size()){selected=&size;break;}
  if(!selected)return std::unexpected(QStringLiteral("Data exceeds the ECC200 capacity"));
  pad(words,selected->dataWords()); words=addEcc(std::move(words),*selected);
  Layout layout(*selected);layout.place(words);return layout.merge();
}

QByteArray Detail::dataMatrixCodewords(const QByteArray& data,bool gs1) {
  return encodeAscii(data,gs1);
}

std::expected<QVector<bool>,QString> ean13(QString data,QString* normalized) {
  data.removeIf([](QChar c){return !c.isDigit();});
  if(data.size()==14&&data.front()==u'0')data.removeFirst();
  if(data.size()==12)data.append(QChar(u'0'+eanCheck(data)));
  if(data.size()!=13)return std::unexpected(QStringLiteral("EAN-13 requires 12 or 13 digits"));
  if(data.back().digitValue()!=eanCheck(QStringView{data}.first(12)))return std::unexpected(QStringLiteral("EAN-13 check digit is invalid"));
  static constexpr std::array<const char*,10> leftOdd{"0001101","0011001","0010011","0111101","0100011","0110001","0101111","0111011","0110111","0001011"};
  static constexpr std::array<const char*,10> leftEven{"0100111","0110011","0011011","0100001","0011101","0111001","0000101","0010001","0001001","0010111"};
  static constexpr std::array<const char*,10> right{"1110010","1100110","1101100","1000010","1011100","1001110","1010000","1000100","1001000","1110100"};
  static constexpr std::array<const char*,10> parity{"OOOOOO","OOEOEE","OOEEOE","OOEEEO","OEOOEE","OEEOOE","OEEEOO","OEOEOE","OEOEEO","OEEOEO"};
  QByteArray pattern="101";const auto p=parity[data[0].digitValue()];
  for(int i=1;i<=6;++i)pattern+=p[i-1]=='O'?leftOdd[data[i].digitValue()]:leftEven[data[i].digitValue()];
  pattern+="01010";for(int i=7;i<13;++i)pattern+=right[data[i].digitValue()];pattern+="101";
  QVector<bool> modules;modules.reserve(95);for(const auto bit:pattern)modules.append(bit=='1');
  if(normalized)*normalized=data;return modules;
}

std::expected<QVector<int>,QString> Detail::code128Codewords(QStringView data,QChar mode) {
  if(data.isEmpty())return std::unexpected(QStringLiteral("Code 128 requires field data"));
  mode=mode.toUpper();
  if(mode==u'D')return code128Gs1(data);
  if(!QStringView{u"NABC"}.contains(mode))
    return std::unexpected(QStringLiteral("Code 128 mode %1 is not implemented").arg(mode));
  const bool hasInvocation=[&]{for(qsizetype i=0;i+1<data.size();++i)if(data[i]==u'>'&&QStringView{u"<0=123456789:;"}.contains(data[i+1]))return true;return false;}();
  if(hasInvocation)return code128Explicit(data);
  switch(mode.toUpper().unicode()){
    case u'A': return code128Auto(data);
    case u'C': {
      if(data.size()%2!=0||!std::ranges::all_of(data,code128Digit))
        return std::unexpected(QStringLiteral("Code 128 subset C requires an even number of digits"));
      QVector<int> words{startC};for(qsizetype i=0;i<data.size();i+=2)words.append(data[i].digitValue()*10+data[i+1].digitValue());
      appendCode128Checksum(words);return words;
    }
    case u'N': case u'B': {
      QVector<int> words{startB};
      for(const auto c:data){auto value=code128DataValue(c,'B');if(!value)return std::unexpected(value.error());words.append(*value);}
      appendCode128Checksum(words);return words;
    }
    default:return std::unexpected(QStringLiteral("Code 128 mode %1 is not implemented").arg(mode));
  }
}

std::expected<QVector<bool>,QString> code128(QString data,QChar mode) {
  auto words=Detail::code128Codewords(data,mode);if(!words)return std::unexpected(words.error());
  QVector<bool> modules;modules.reserve((words->size()*11)+13);
  for(const int word:*words)for(const char* p=code128Patterns[word];*p;++p){const int width=*p-'0';const bool bar=(p-code128Patterns[word])%2==0;for(int i=0;i<width;++i)modules.append(bar);}
  for(const char* p=code128Patterns.back();*p;++p){const int width=*p-'0';const bool bar=(p-code128Patterns.back())%2==0;for(int i=0;i<width;++i)modules.append(bar);}
  return modules;
}

std::expected<QVector<bool>,QString> codabar(QStringView data,int wideToNarrow) {
  static constexpr std::string_view alphabet="0123456789-$:/.+ABCD";
  static constexpr std::array<unsigned int,20> encodings{
    0x003,0x006,0x009,0x060,0x012,0x042,0x021,0x024,0x030,0x048,
    0x00c,0x018,0x045,0x051,0x054,0x015,0x01a,0x029,0x00b,0x00e};
  if(data.size()<2)return std::unexpected(QStringLiteral("Codabar requires start and stop characters"));
  const auto normalized=data.toString().toUpper();
  if(!QStringView{u"ABCD"}.contains(normalized.front())||!QStringView{u"ABCD"}.contains(normalized.back()))
    return std::unexpected(QStringLiteral("Codabar data must start and end with A, B, C, or D"));
  const int wide=std::clamp(wideToNarrow,2,3);
  QVector<bool> modules;
  for(qsizetype character=0;character<normalized.size();++character){
    const auto index=alphabet.find(static_cast<char>(normalized[character].toLatin1()));
    if(index==std::string_view::npos)return std::unexpected(QStringLiteral("Codabar contains an invalid character"));
    const unsigned int pattern=encodings[index];
    for(int element=0;element<7;++element){
      const int width=(pattern&(1U<<(6-element)))?wide:1;
      for(int module=0;module<width;++module)modules.append(element%2==0);
    }
    if(character+1<normalized.size())modules.append(false);
  }
  return modules;
}

} // namespace QtZpl::BarcodeEncoders
