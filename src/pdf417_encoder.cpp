#include "pdf417_encoder.hpp"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <QtCore/QByteArrayView>

namespace QtZpl::BarcodeEncoders::Pdf417 {
namespace {

constexpr int startWord=0x1fea8;
constexpr int stopWord=0x3fa29;
constexpr int paddingCodeword=900;
constexpr int minNumericCount=13;

#include "pdf417_codeword_patterns.inc"

enum class EncodingMode { Text, Numeric, Binary };
enum class SubMode { Upper, Lower, Mixed, Punctuation };

constexpr std::array<unsigned char,30> mixedRaw{
  '0','1','2','3','4','5','6','7','8','9','&','\r','\t',',',':','#','-','.','$','/','+','%','*','=','^',0,' ',0,0,0};
constexpr std::array<unsigned char,30> punctuationRaw{
  ';','<','>','@','[','\\',']','_','`','~','!','\r','\t',',',':','\n','-','.','$','/','"','|','*','(',')','?','{','}','\'',0};

int tableIndex(const std::array<unsigned char,30>& table,unsigned char value) {
  const auto found=std::find(table.cbegin(),table.cend(),value);
  return found==table.cend()?-1:static_cast<int>(found-table.cbegin());
}

int digitCount(QByteArrayView data,qsizetype position) {
  int count=0;
  while(position+count<data.size()&&data[position+count]>='0'&&data[position+count]<='9')++count;
  return count;
}

bool isTextByte(unsigned char value) {
  return value=='\t'||value=='\n'||value=='\r'||(value>=32&&value<=126);
}

int textCount(QByteArrayView data,qsizetype position) {
  int result=0;
  while(position+result<data.size()){
    const int numeric=digitCount(data,position+result);
    const auto value=static_cast<unsigned char>(data[position+result]);
    if(numeric>=minNumericCount||(numeric==0&&!isTextByte(value)))break;
    ++result;
  }
  return result;
}

int binaryCount(QByteArrayView data,qsizetype position) {
  int result=0;
  while(position+result<data.size()){
    if(digitCount(data,position+result)>=minNumericCount)break;
    if(textCount(data,position+result)>5)break;
    ++result;
  }
  return result;
}

std::expected<QVector<int>,QString> encodeNumeric(QByteArrayView digits) {
  QVector<int> result;
  for(qsizetype start=0;start<digits.size();start+=44){
    const auto chunk=digits.sliced(start,std::min<qsizetype>(44,digits.size()-start));
    QByteArray decimal("1");decimal.append(chunk.data(),chunk.size());
    QVector<int> words;
    while(decimal!=QByteArrayLiteral("0")){
      QByteArray quotient;quotient.reserve(decimal.size());int remainder=0;
      for(const char character:decimal){
        if(character<'0'||character>'9')return std::unexpected(QStringLiteral("PDF417 numeric compaction requires digits"));
        const int value=remainder*10+(character-'0');
        if(!quotient.isEmpty()||value/900>0)quotient.append(char('0'+value/900));
        remainder=value%900;
      }
      words.prepend(remainder);decimal=quotient.isEmpty()?QByteArrayLiteral("0"):quotient;
    }
    result+=words;
  }
  return result;
}

std::expected<QVector<int>,QString> encodeText(QByteArrayView text,SubMode& submode) {
  QVector<int> temporary;
  qsizetype index=0;
  while(index<text.size()){
    const auto character=static_cast<unsigned char>(text[index]);
    const bool upper=character==' '||(character>='A'&&character<='Z');
    const bool lower=character==' '||(character>='a'&&character<='z');
    const int mixed=tableIndex(mixedRaw,character);
    const int punctuation=tableIndex(punctuationRaw,character);
    switch(submode){
      case SubMode::Upper:
        if(upper)temporary.append(character==' '?26:character-'A');
        else if(lower){submode=SubMode::Lower;temporary.append(27);continue;}
        else if(mixed>=0){submode=SubMode::Mixed;temporary.append(28);continue;}
        else if(punctuation>=0){temporary.append(29);temporary.append(punctuation);}
        else return std::unexpected(QStringLiteral("PDF417 text compaction encountered an unsupported character"));
        break;
      case SubMode::Lower:
        if(lower)temporary.append(character==' '?26:character-'a');
        else if(upper){temporary.append(27);temporary.append(character-'A');}
        else if(mixed>=0){submode=SubMode::Mixed;temporary.append(28);continue;}
        else if(punctuation>=0){temporary.append(29);temporary.append(punctuation);}
        else return std::unexpected(QStringLiteral("PDF417 text compaction encountered an unsupported character"));
        break;
      case SubMode::Mixed:
        if(mixed>=0)temporary.append(mixed);
        else if(upper){submode=SubMode::Upper;temporary.append(28);continue;}
        else if(lower){submode=SubMode::Lower;temporary.append(27);continue;}
        else if(punctuation>=0){
          const int next=index+1<text.size()?tableIndex(punctuationRaw,static_cast<unsigned char>(text[index+1])):-1;
          if(next>=0){submode=SubMode::Punctuation;temporary.append(25);continue;}
          temporary.append(29);temporary.append(punctuation);
        }else return std::unexpected(QStringLiteral("PDF417 text compaction encountered an unsupported character"));
        break;
      case SubMode::Punctuation:
        if(punctuation>=0)temporary.append(punctuation);
        else{submode=SubMode::Upper;temporary.append(29);continue;}
        break;
    }
    ++index;
  }
  QVector<int> result;
  for(qsizetype i=0;i<temporary.size();i+=2){
    const int second=i+1<temporary.size()?temporary[i+1]:29;
    result.append(temporary[i]*30+second);
  }
  return result;
}

QVector<int> encodeBinary(QByteArrayView data,EncodingMode startMode) {
  QVector<int> result;
  if(data.size()==1&&startMode==EncodingMode::Text)result.append(913);
  else if(data.size()%6==0)result.append(924);
  else result.append(901);
  qsizetype index=0;
  while(data.size()-index>=6){
    std::uint64_t value=0;
    for(int i=0;i<6;++i)value=(value<<8)|static_cast<unsigned char>(data[index+i]);
    std::array<int,5> words{};
    for(int i=4;i>=0;--i){words[i]=static_cast<int>(value%900);value/=900;}
    for(const int word:words)result.append(word);
    index+=6;
  }
  while(index<data.size())result.append(static_cast<unsigned char>(data[index++]));
  return result;
}

int errorWordCount(int level) { return 1<<(level+1); }

int powerMod(int base,int exponent) {
  int result=1;
  while(exponent>0){if(exponent&1)result=(result*base)%929;base=(base*base)%929;exponent>>=1;}
  return result;
}

QVector<int> correctionFactors(int count) {
  QVector<int> polynomial{1};
  for(int i=1;i<=count;++i){
    const int root=powerMod(3,i);QVector<int> next(polynomial.size()+1);
    for(qsizetype j=0;j<polynomial.size();++j){
      next[j]=(next[j]+polynomial[j])%929;
      next[j+1]=(next[j+1]+929-(polynomial[j]*root)%929)%929;
    }
    polynomial=std::move(next);
  }
  QVector<int> factors;factors.reserve(count);
  for(int i=count;i>=1;--i)factors.append(polynomial[i]);
  return factors;
}

std::optional<QPair<int,int>> resolveDimensions(int dataWords,int ecWords,int columns,int rows) {
  constexpr int minColumns=1,maxColumns=30,minRows=3,maxRows=90;
  const auto calculatedRows=[&](int c){int r=((dataWords+1+ecWords)/c)+1;if(c*r>=dataWords+1+ecWords+c)--r;return r;};
  if(columns==0&&rows==0){
    double bestRatio=0.0;int bestColumns=0,bestRows=0;
    for(int c=minColumns;c<=maxColumns;++c){
      const int r=calculatedRows(c);if(r<minRows)break;if(r>maxRows)continue;
      const double ratio=static_cast<double>(17*c+69)/static_cast<double>(r*2);
      if(bestRows!=0&&std::abs(ratio-3.0)>std::abs(bestRatio-3.0))continue;
      bestRatio=ratio;bestColumns=c;bestRows=r;
    }
    if(bestRows==0&&calculatedRows(minColumns)<minRows)return QPair{minColumns,minRows};
    if(bestRows==0)return std::nullopt;
    return QPair{bestColumns,bestRows};
  }
  const int total=dataWords+ecWords+1;
  if(columns>0&&rows==0)rows=(total+columns-1)/columns;
  else if(columns==0&&rows>0)columns=(total+rows-1)/rows;
  if(columns<minColumns||columns>maxColumns||rows<minRows||rows>maxRows||columns*rows<total)return std::nullopt;
  return QPair{columns,rows};
}

int leftCodeword(int row,int rows,int columns,int level) {
  int value=0;
  switch(row%3){case 0:value=(rows-1)/3;break;case 1:value=level*3+(rows-1)%3;break;default:value=columns-1;break;}
  return 30*(row/3)+value;
}

int rightCodeword(int row,int rows,int columns,int level) {
  int value=0;
  switch(row%3){case 0:value=columns-1;break;case 1:value=(rows-1)/3;break;default:value=level*3+(rows-1)%3;break;}
  return 30*(row/3)+value;
}

void appendPattern(Matrix& matrix,int row,int& x,int pattern,int bits) {
  for(int bit=bits-1;bit>=0;--bit)matrix.modules[row*matrix.width+x++]=(pattern&(1<<bit))!=0;
}

} // namespace

std::expected<QVector<int>,QString> highLevelCodewords(const QByteArray& data) {
  QVector<int> result;EncodingMode mode=EncodingMode::Text;SubMode submode=SubMode::Upper;
  qsizetype position=0;
  while(position<data.size()){
    const int numeric=digitCount(data,position);
    if(numeric>=minNumericCount||numeric==data.size()-position){
      result.append(902);mode=EncodingMode::Numeric;submode=SubMode::Upper;
      auto encoded=encodeNumeric(QByteArrayView{data}.sliced(position,numeric));if(!encoded)return std::unexpected(encoded.error());
      result+=*encoded;position+=numeric;continue;
    }
    const int text=textCount(data,position);
    if(text>=5||text==data.size()-position){
      if(mode!=EncodingMode::Text){result.append(900);mode=EncodingMode::Text;submode=SubMode::Upper;}
      auto encoded=encodeText(QByteArrayView{data}.sliced(position,text),submode);if(!encoded)return std::unexpected(encoded.error());
      result+=*encoded;position+=text;continue;
    }
    int binary=binaryCount(data,position);if(binary==0)binary=1;
    if(binary!=1||mode!=EncodingMode::Text){mode=EncodingMode::Binary;submode=SubMode::Upper;}
    result+=encodeBinary(QByteArrayView{data}.sliced(position,binary),mode);position+=binary;
  }
  return result;
}

QVector<int> errorCorrection(const QVector<int>& data,int securityLevel) {
  const int count=errorWordCount(securityLevel);const auto factors=correctionFactors(count);QVector<int> words(count);
  for(const int value:data){
    const int temporary=(value+words[0])%929;
    for(int i=count-1;i>=0;--i){const int add=i>0?words[count-i]:0;words[count-1-i]=(add+929-(temporary*factors[i])%929)%929;}
  }
  for(int& word:words)if(word>0)word=929-word;
  return words;
}

std::expected<Symbol,QString> encode(const QByteArray& data,int securityLevel,int columns,int rows) {
  if(securityLevel<0||securityLevel>8)return std::unexpected(QStringLiteral("PDF417 security level must be between 0 and 8"));
  auto highLevel=highLevelCodewords(data);if(!highLevel)return std::unexpected(highLevel.error());
  const int ecCount=errorWordCount(securityLevel);const auto dimensions=resolveDimensions(highLevel->size(),ecCount,columns,rows);
  if(!dimensions)return std::unexpected(QStringLiteral("PDF417 data does not fit the requested dimensions"));
  columns=dimensions->first;rows=dimensions->second;
  QVector<int> dataWords=*highLevel;
  const int padding=columns*rows-(dataWords.size()+ecCount+1);
  for(int i=0;i<padding;++i)dataWords.append(paddingCodeword);
  dataWords.prepend(dataWords.size()+1);
  QVector<int> all=dataWords;all+=errorCorrection(dataWords,securityLevel);
  Matrix matrix{(columns+4)*17+1,rows,QVector<bool>(((columns+4)*17+1)*rows)};
  for(int row=0;row<rows;++row){
    const int cluster=row%3;int x=0;appendPattern(matrix,row,x,startWord,17);
    appendPattern(matrix,row,x,pdf417CodewordPatterns[cluster][leftCodeword(row,rows,columns,securityLevel)],17);
    for(int column=0;column<columns;++column)appendPattern(matrix,row,x,pdf417CodewordPatterns[cluster][all[row*columns+column]],17);
    appendPattern(matrix,row,x,pdf417CodewordPatterns[cluster][rightCodeword(row,rows,columns,securityLevel)],17);
    appendPattern(matrix,row,x,stopWord,18);
  }
  return Symbol{std::move(matrix),std::move(dataWords),std::move(all),columns,rows};
}

} // namespace QtZpl::BarcodeEncoders::Pdf417
