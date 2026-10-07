#include "maxicode_encoder.hpp"

#include <algorithm>
#include <array>

namespace QtZpl::BarcodeEncoders::MaxiCode {
namespace {

constexpr char recordSeparator=30;
constexpr char groupSeparator=29;
constexpr char endOfTransmission=4;

#include "maxicode_tables.inc"

class ReedSolomon final {
public:
  explicit ReedSolomon(int symbols) : symbols_(symbols), logTable_(64), antiLogTable_(63), polynomial_(symbols+1) {
    constexpr int primitive=0x43;int topBit=1,size=0;
    for(;topBit<=primitive;topBit<<=1)++size;
    --size;topBit>>=1;logSize_=(1<<size)-1;
    for(int value=1,index=0;index<logSize_;++index){antiLogTable_[index]=value;logTable_[value]=index;value<<=1;if(value&topBit)value^=primitive;}
    polynomial_[0]=1;int root=1;
    for(int i=1;i<=symbols_;++i){
      polynomial_[i]=1;
      for(int k=i-1;k>0;--k){if(polynomial_[k]!=0)polynomial_[k]=antiLogTable_[(logTable_[polynomial_[k]]+root)%logSize_];polynomial_[k]^=polynomial_[k-1];}
      polynomial_[0]=antiLogTable_[(logTable_[polynomial_[0]]+root)%logSize_];++root;
    }
  }

  QVector<int> encode(const QVector<int>& data) const {
    QVector<int> ecc(symbols_);
    for(const int value:data){
      const int feedback=ecc[symbols_-1]^value;
      for(int j=symbols_-1;j>0;--j){
        if(feedback!=0&&polynomial_[j]!=0)ecc[j]=ecc[j-1]^antiLogTable_[(logTable_[feedback]+logTable_[polynomial_[j]])%logSize_];
        else ecc[j]=ecc[j-1];
      }
      ecc[0]=feedback!=0&&polynomial_[0]!=0?antiLogTable_[(logTable_[feedback]+logTable_[polynomial_[0]])%logSize_]:0;
    }
    return ecc;
  }

private:
  int symbols_=0;int logSize_=0;QVector<int> logTable_;QVector<int> antiLogTable_;QVector<int> polynomial_;
};

int determineSet(const std::array<int,144>& sets,int index,std::initializer_list<int> allowed) {
  const int previous=sets[index-1],next=sets[index+1];
  const bool previousAllowed=std::ranges::find(allowed,previous)!=allowed.end();
  const bool nextAllowed=std::ranges::find(allowed,next)!=allowed.end();
  if(previousAllowed&&nextAllowed)return std::min(previous,next);
  if(previousAllowed)return previous;if(nextAllowed)return next;return *allowed.begin();
}

void insertPosition(std::array<int,144>& sets,std::array<int,144>& characters,int position,int& dataLength) {
  for(int i=143;i>position;--i){sets[i]=sets[i-1];characters[i]=characters[i-1];}
  ++dataLength;
}

std::expected<QVector<int>,QString> secondaryCodewords(const QByteArray& data, int capacity = 84) {
  if(data.isEmpty())return std::unexpected(QStringLiteral("MaxiCode secondary data is empty"));
  if(data.size()>138)return std::unexpected(QStringLiteral("MaxiCode secondary data is too long"));
  std::array<int,144> sets{};std::array<int,144> characters{};
  for(qsizetype i=0;i<data.size();++i){const auto value=static_cast<unsigned char>(data[i]);sets[i]=maxiCodeSet[value];characters[i]=maxiSymbolChar[value];}
  if(sets[0]==0){if(characters[0]==13)characters[0]=0;sets[0]=1;}
  int dataLength=static_cast<int>(data.size());
  for(int i=1;i<dataLength;++i){
    if(sets[i]!=0)continue;
    switch(characters[i]){
      case 13:sets[i]=determineSet(sets,i,{1,5});characters[i]=sets[i]==5?13:0;break;
      case 28:sets[i]=determineSet(sets,i,{1,2,3,4,5});if(sets[i]==5)characters[i]=32;break;
      case 29:sets[i]=determineSet(sets,i,{1,2,3,4,5});if(sets[i]==5)characters[i]=33;break;
      case 30:sets[i]=determineSet(sets,i,{1,2,3,4,5});if(sets[i]==5)characters[i]=34;break;
      case 32:
        sets[i]=determineSet(sets,i,{1,2,3,4,5});characters[i]=sets[i]==1?32:(sets[i]==2?47:59);break;
      case 44:sets[i]=determineSet(sets,i,{1,2});if(sets[i]==2)characters[i]=48;break;
      case 46:sets[i]=determineSet(sets,i,{1,2});if(sets[i]==2)characters[i]=49;break;
      case 47:sets[i]=determineSet(sets,i,{1,2});if(sets[i]==2)characters[i]=50;break;
      case 58:sets[i]=determineSet(sets,i,{1,2});if(sets[i]==2)characters[i]=51;break;
      default:break;
    }
  }
  const int paddingSet=sets[dataLength-1]==2?2:1;
  for(int i=dataLength;i<144;++i){sets[i]=paddingSet;characters[i]=33;}

  int digitRun=0;
  for(int i=0;i<144;++i){
    if(sets[i]==1&&characters[i]>=48&&characters[i]<=57)++digitRun;else digitRun=0;
    if(digitRun==9){for(int j=i-8;j<=i;++j)sets[j]=6;digitRun=0;}
  }

  int currentSet=1,index=0;
  while(index<144){
    if(sets[index]!=currentSet&&sets[index]!=6){
      switch(sets[index]){
        case 1:
          if(currentSet==2){
            if(index+1<144&&sets[index+1]==1){
              if(index+2<144&&sets[index+2]==1){
                if(index+3<144&&sets[index+3]==1){insertPosition(sets,characters,index,dataLength);characters[index]=63;currentSet=1;index+=3;}
                else{insertPosition(sets,characters,index,dataLength);characters[index]=57;index+=2;}
              }else{insertPosition(sets,characters,index,dataLength);characters[index]=56;++index;}
            }else{insertPosition(sets,characters,index,dataLength);characters[index]=59;}
          }else{insertPosition(sets,characters,index,dataLength);characters[index]=58;currentSet=1;}
          break;
        case 2:
          if(currentSet!=1||(index+1<144&&sets[index+1]==2)){insertPosition(sets,characters,index,dataLength);characters[index]=63;currentSet=2;}
          else{insertPosition(sets,characters,index,dataLength);characters[index]=59;}
          break;
        case 3:case 4:case 5:{
          const bool latch=(index==0&&index+3<144&&sets[index+1]==sets[index]&&sets[index+2]==sets[index]&&sets[index+3]==sets[index])
            ||(index>0&&sets[index-1]==sets[index]&&index+2<144&&sets[index+1]==sets[index]&&sets[index+2]==sets[index]);
          if(latch){
            if(index==0){insertPosition(sets,characters,index,dataLength);characters[index]=60+sets[index]-3;++index;insertPosition(sets,characters,index,dataLength);characters[index]=60+sets[index]-3;index+=3;}
            else{insertPosition(sets,characters,index,dataLength);characters[index-1]=60+sets[index]-3;index+=2;}
            currentSet=sets[index];
          }else{insertPosition(sets,characters,index,dataLength);characters[index]=60+sets[index]-3;}
          break;
        }
        default:break;
      }
      ++index;
    }
    ++index;
  }

  index=0;
  while(index<144){
    if(sets[index]==6&&index+9<=144){
      int value=0;for(int j=0;j<9;++j)value=value*10+(characters[index+j]-48);
      characters[index]=31;characters[index+1]=(value&0x3f000000)>>24;characters[index+2]=(value&0xfc0000)>>18;
      characters[index+3]=(value&0x3f000)>>12;characters[index+4]=(value&0xfc0)>>6;characters[index+5]=value&0x3f;
      index+=6;for(int j=index;j<141;++j){sets[j]=sets[j+3];characters[j]=characters[j+3];}dataLength-=3;
    }else ++index;
  }
  if(dataLength>capacity)return std::unexpected(QStringLiteral("MaxiCode data exceeds the selected mode capacity"));
  QVector<int> output(capacity);std::copy_n(characters.cbegin(),capacity,output.begin());return output;
}

void addErrorCorrection(QVector<int>& codewords, bool enhanced = false) {
  // The field tables and generator polynomials depend only on the ECC length.
  // Keep them immutable and share them across symbols and rendering threads.
  static const ReedSolomon primaryEncoder(10),secondaryEncoder(20),enhancedEncoder(28);
  QVector<int> primary;for(int i=0;i<10;++i)primary.append(codewords[i]);const auto primaryEcc=primaryEncoder.encode(primary);
  for(int i=0;i<10;++i)codewords[10+i]=primaryEcc[9-i];
  const int dataLength=enhanced?68:84,eccLength=enhanced?28:20;
  const auto& encoder=enhanced?enhancedEncoder:secondaryEncoder;
  QVector<int> even,odd;for(int i=0;i<dataLength;i+=2){even.append(codewords[20+i]);odd.append(codewords[21+i]);}
  const auto evenEcc=encoder.encode(even);const auto oddEcc=encoder.encode(odd);
  for(int i=0;i<eccLength;++i){codewords[20+dataLength+2*i]=evenEcc[eccLength-1-i];codewords[21+dataLength+2*i]=oddEcc[eccLength-1-i];}
}

Symbol makeSymbol(QVector<int> codewords) {
  Matrix grid{30,33,QVector<bool>(30*33)};
  for(int row=0;row<33;++row)for(int column=0;column<30;++column){const int sequence=maxiGrid[row*30+column]+5;const int block=sequence/6,bit=sequence%6;if(block>0)grid.modules[row*30+column]=(codewords[block-1]&(32>>bit))!=0;}
  const auto mark=[&](int row,int column){grid.modules[row*30+column]=true;};
  mark(0,28);mark(0,29);mark(9,10);mark(9,11);mark(10,11);mark(15,7);mark(16,8);mark(16,20);mark(17,20);mark(22,10);mark(23,10);mark(22,17);mark(23,17);
  return Symbol{std::move(grid),std::move(codewords)};
}

bool decimalBytes(const QByteArray& data) {
  return !data.isEmpty() && std::ranges::all_of(data, [](char c) { return c >= '0' && c <= '9'; });
}

} // namespace

std::expected<QByteArray,QString> reconstructMode2(const QByteArray& data) {
  const QByteArray header=QByteArrayLiteral("[)>")+recordSeparator+QByteArrayLiteral("01")+groupSeparator;
  const int headerIndex=data.indexOf(header);
  if(headerIndex<=0)return std::unexpected(QStringLiteral("MaxiCode mode 2 data has no structured carrier header"));
  const auto primary=data.first(headerIndex);if(primary.size()!=15&&primary.size()!=11)return std::unexpected(QStringLiteral("MaxiCode mode 2 primary prefix has invalid length"));
  const auto service=primary.first(3);const auto country=primary.sliced(3,3);
  auto postcode=primary.sliced(6);if(postcode.size()==5)postcode+=QByteArrayLiteral("0000");
  const QByteArray fullHeader=header+QByteArrayLiteral("96");const auto secondary=data.sliced(headerIndex);
  if(!secondary.startsWith(fullHeader))return std::unexpected(QStringLiteral("MaxiCode mode 2 data has an invalid format identifier"));
  return fullHeader+postcode+groupSeparator+country+groupSeparator+service+groupSeparator+secondary.sliced(fullHeader.size());
}

std::expected<Symbol,QString> encodeMode2(const QByteArray& zplData) {
  auto rebuilt=reconstructMode2(zplData);if(!rebuilt)return std::unexpected(rebuilt.error());
  const QByteArray header=QByteArrayLiteral("[)>")+recordSeparator+QByteArrayLiteral("01")+groupSeparator;
  if(!rebuilt->startsWith(header)||!rebuilt->endsWith(endOfTransmission))return std::unexpected(QStringLiteral("MaxiCode structured carrier envelope is invalid"));
  const auto groups=rebuilt->sliced(9).split(groupSeparator);if(groups.size()<3)return std::unexpected(QStringLiteral("MaxiCode primary fields are missing"));
  const auto postcode=groups[0],countryText=groups[1],serviceText=groups[2];
  bool countryOk=false,serviceOk=false,postcodeOk=false;const int country=countryText.toInt(&countryOk);const int service=serviceText.toInt(&serviceOk);const int postal=postcode.toInt(&postcodeOk);
  if(postcode.size()!=9||!postcodeOk||countryText.size()!=3||!countryOk||serviceText.size()!=3||!serviceOk)return std::unexpected(QStringLiteral("MaxiCode mode 2 primary fields are invalid"));
  QVector<int> codewords(144);constexpr int postcodeLength=9;
  codewords[0]=((postal&0x03)<<4)|2;codewords[1]=(postal&0xfc)>>2;codewords[2]=(postal&0x3f00)>>8;codewords[3]=(postal&0xfc000)>>14;
  codewords[4]=(postal&0x3f00000)>>20;codewords[5]=((postal&0x3c000000)>>26)|((postcodeLength&0x3)<<4);
  codewords[6]=((postcodeLength&0x3c)>>2)|((country&0x3)<<4);codewords[7]=(country&0xfc)>>2;
  codewords[8]=((country&0x300)>>8)|((service&0xf)<<2);codewords[9]=(service&0x3f0)>>4;
  QByteArray secondary=rebuilt->first(9);if(groups.size()>3){for(qsizetype i=3;i<groups.size();++i){if(i>3)secondary.append(groupSeparator);secondary+=groups[i];}}
  auto secondaryWords=secondaryCodewords(secondary);if(!secondaryWords)return std::unexpected(secondaryWords.error());
  for(int i=0;i<84;++i)codewords[20+i]=(*secondaryWords)[i];addErrorCorrection(codewords);
  return makeSymbol(std::move(codewords));
}

std::expected<Symbol,QString> encode(const QByteArray& zplData, int mode, int position, int total) {
  if(mode<2||mode>6)return std::unexpected(QStringLiteral("MaxiCode mode must be 2 through 6"));
  if(total<1||total>8||position<1||position>total)return std::unexpected(QStringLiteral("MaxiCode structured append requires 1 <= position <= total <= 8"));
  if(zplData.isEmpty())return std::unexpected(QStringLiteral("MaxiCode data is empty"));
  QVector<int> codewords(144);
  QByteArray message;
  if(mode<=3){
    const QByteArray header=QByteArrayLiteral("[)>")+recordSeparator+QByteArrayLiteral("01")+groupSeparator+QByteArrayLiteral("96");
    const qsizetype primaryLength=zplData.indexOf(header);
    const bool validLength=mode==2?(primaryLength==11||primaryLength==15):primaryLength==12;
    if(!validLength||!zplData.endsWith(endOfTransmission))return std::unexpected(QStringLiteral("MaxiCode primary prefix or structured carrier envelope is invalid"));
    const auto serviceText=zplData.first(3),countryText=zplData.sliced(3,3);
    if(!decimalBytes(serviceText)||!decimalBytes(countryText))return std::unexpected(QStringLiteral("MaxiCode country and service must each contain three digits"));
    const int country=countryText.toInt(),service=serviceText.toInt();
    auto postcode=zplData.sliced(6,primaryLength-6);
    if(mode==2){
      if(!decimalBytes(postcode))return std::unexpected(QStringLiteral("MaxiCode mode 2 postal code must contain only digits"));
      if(postcode.size()==5)postcode+=QByteArrayLiteral("0000");
      const int postal=postcode.toInt(),length=static_cast<int>(postcode.size());
      codewords[0]=((postal&3)<<4)|2;codewords[1]=(postal&0xfc)>>2;codewords[2]=(postal&0x3f00)>>8;codewords[3]=(postal&0xfc000)>>14;
      codewords[4]=(postal&0x3f00000)>>20;codewords[5]=((postal&0x3c000000)>>26)|((length&3)<<4);
      codewords[6]=(length>>2)|((country&3)<<4);
    }else{
      // Zebra permits uppercase letters, digits and trailing padding spaces.
      bool padding=false;
      std::array<int,6> postal{};
      for(int i=0;i<6;++i){
        const char c=postcode[i];
        if(c==' ')padding=true;
        else if(padding||!((c>='A'&&c<='Z')||(c>='0'&&c<='9')))
          return std::unexpected(QStringLiteral("MaxiCode mode 3 postal code requires six uppercase letters/digits, with optional trailing spaces"));
        postal[i]=maxiSymbolChar[static_cast<unsigned char>(c)];
      }
      codewords[0]=((postal[5]&3)<<4)|3;
      for(int i=1;i<6;++i)codewords[i]=((postal[5-i]&3)<<4)|((postal[6-i]&0x3c)>>2);
      codewords[6]=((postal[0]&0x3c)>>2)|((country&3)<<4);
    }
    codewords[7]=(country&0xfc)>>2;
    codewords[8]=((country&0x300)>>8)|((service&15)<<2);
    codewords[9]=(service&0x3f0)>>4;
    message=zplData.sliced(primaryLength);
  }else{
    codewords[0]=mode;
    message=zplData;
  }
  const int capacity=mode<=3?84:mode==5?77:93;
  const int appendWords=total>1?2:0;
  auto words=secondaryCodewords(message,capacity-appendWords);
  if(!words)return std::unexpected(words.error());
  if(appendWords){words->prepend(((position-1)<<3)|(total-1));words->prepend(33);}
  if(mode<=3){for(int i=0;i<84;++i)codewords[20+i]=(*words)[i];}
  else{
    for(int i=0;i<9;++i)codewords[1+i]=(*words)[i];
    for(int i=9;i<capacity;++i)codewords[11+i]=(*words)[i];
  }
  addErrorCorrection(codewords,mode==5);
  return makeSymbol(std::move(codewords));
}

} // namespace QtZpl::BarcodeEncoders::MaxiCode
