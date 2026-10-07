#include "linear_encoders.hpp"
#include <array>
#include <algorithm>
#include <string_view>

namespace QtZpl::BarcodeEncoders::Linear {
namespace {
using namespace Qt::StringLiterals;
constexpr std::array<std::string_view,10> odd{"0001101","0011001","0010011","0111101","0100011","0110001","0101111","0111011","0110111","0001011"};
constexpr std::array<std::string_view,10> even{"0100111","0110011","0011011","0100001","0011101","0111001","0000101","0010001","0001001","0010111"};
constexpr std::array<std::string_view,10> right{"1110010","1100110","1101100","1000010","1011100","1001110","1010000","1000100","1001000","1110100"};
constexpr std::array<std::string_view,10> twoOfFive{"00110","10001","01001","11000","00101","10100","01100","00011","10010","01010"};
bool digits(QStringView value){return !value.isEmpty()&&std::ranges::all_of(value,[](QChar c){return c>=u'0'&&c<=u'9';});}
QChar mod10(QStringView value){int sum=0,weight=3;for(qsizetype i=value.size();i>0;){sum+=(value[--i].unicode()-u'0')*weight;weight=4-weight;}return QChar(u'0'+(10-sum%10)%10);}
void append(QVector<bool>& target,std::string_view pattern){for(char c:pattern)target.append(c=='1');}
void run(QVector<bool>& target,bool bar,int width){for(int i=0;i<width;++i)target.append(bar);}

std::expected<QString,QString> gtin(QStringView value,int length) {
  if(!digits(value)|| (value.size()!=length-1&&value.size()!=length))
    return std::unexpected(u"Expected %1 or %2 ASCII digits"_s.arg(length-1).arg(length));
  QString result=value.toString();
  if(result.size()==length-1)result+=mod10(result);
  else if(result.back()!=mod10(value.first(length-1)))return std::unexpected(u"Invalid GTIN check digit"_s);
  return result;
}

std::expected<Symbol,QString> retail(QStringView symbology,QStringView value) {
  Symbol result;
  if(symbology==u"B9"){
    if(!digits(value)||(value.size()!=6&&value.size()!=7&&value.size()!=8))return std::unexpected(u"UPC-E expects 6, 7 or 8 ASCII digits"_s);
    QString data=value.toString();if(data.size()==6)data.prepend(u'0');
    if(data.front()!=u'0'&&data.front()!=u'1')return std::unexpected(u"UPC-E number system must be 0 or 1"_s);
    const QString body=data.mid(1,6);QString full=data.left(1);
    switch(body.back().unicode()){
      case '0':case '1':case '2':full+=body.first(2)+body.last(1)+u"0000"_s+body.mid(2,3);break;
      case '3':full+=body.first(3)+u"00000"_s+body.mid(3,2);break;
      case '4':full+=body.first(4)+u"00000"_s+body.mid(4,1);break;
      default:full+=body.first(5)+u"0000"_s+body.last(1);break;
    }
    const auto check=mod10(full);
    if(data.size()==8&&data.back()!=check)return std::unexpected(u"Invalid UPC-E check digit"_s);
    if(data.size()==7)data+=check;
    // ISO/IEC 15420 UPC-E parity, number system 0; system 1 reverses parity.
    constexpr std::array<unsigned,10> parity{0x38,0x34,0x32,0x31,0x2c,0x26,0x23,0x2a,0x29,0x25};
    const unsigned flags=parity[check.unicode()-u'0']^(data.front()==u'1'?0x3f:0);
    append(result.modules,"101");
    for(int i=0;i<6;++i)append(result.modules,(flags&(1U<<(5-i)))?even[body[i].unicode()-u'0']:odd[body[i].unicode()-u'0']);
    append(result.modules,"010101");result.text=data;return result;
  }
  const int count=symbology==u"B8"?8:12;
  auto data=gtin(value,count);if(!data)return std::unexpected(data.error());
  append(result.modules,"101");
  for(int i=0;i<count/2;++i)append(result.modules,odd[(*data)[i].unicode()-u'0']);
  append(result.modules,"01010");
  for(int i=count/2;i<count;++i)append(result.modules,right[(*data)[i].unicode()-u'0']);
  append(result.modules,"101");result.text=*data;return result;
}

// Code 93 encodings/full-ASCII mapping follow ZXing (Apache-2.0).
// Copyright 2010/2016 ZXing authors; see third_party/zxing-linear/LICENSE.
std::expected<Symbol,QString> code93(QStringView data) {
  constexpr std::string_view alphabet="0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ-. $/+%abcd*";
  constexpr std::array<unsigned,48> patterns{0x114,0x148,0x144,0x142,0x128,0x124,0x122,0x150,0x112,0x10a,
    0x1a8,0x1a4,0x1a2,0x194,0x192,0x18a,0x168,0x164,0x162,0x134,0x11a,0x158,0x14c,0x146,0x12c,0x116,0x1b4,0x1b2,0x1ac,0x1a6,
    0x196,0x19a,0x16c,0x166,0x136,0x13a,0x12e,0x1d4,0x1d2,0x1ca,0x16e,0x176,0x1ae,0x126,0x1da,0x1d6,0x132,0x15e};
  QByteArray expanded;
  for(QChar c:data){
    const auto value=c.unicode();
    if(value>127)return std::unexpected(u"Code 93 requires ASCII data"_s);
    const char ch=static_cast<char>(value);
    if(value==0)expanded+="bU";
    else if(value<=26){expanded+='a';expanded+=char('A'+value-1);}
    else if(value<=31){expanded+='b';expanded+=char('A'+value-27);}
    else if(ch==' '||ch=='$'||ch=='%'||ch=='+')expanded+=ch;
    else if(ch<=','){expanded+='c';expanded+=char('A'+ch-'!');}
    else if(ch<='9')expanded+=ch;
    else if(ch==':')expanded+="cZ";
    else if(ch<='?'){expanded+='b';expanded+=char('F'+ch-';');}
    else if(ch=='@')expanded+="bV";
    else if(ch<='Z')expanded+=ch;
    else if(ch<='_'){expanded+='b';expanded+=char('K'+ch-'[');}
    else if(ch=='`')expanded+="bW";
    else if(ch<='z'){expanded+='d';expanded+=char('A'+ch-'a');}
    else{expanded+='b';expanded+=char('P'+ch-'{');}
  }
  Symbol symbol;symbol.text=data.toString();
  for(int maximum:{20,15}){
    int weight=1,total=0;
    for(qsizetype i=expanded.size();i>0;){total+=static_cast<int>(alphabet.find(expanded[--i]))*weight;if(++weight>maximum)weight=1;}
    const char check=alphabet[total%47];expanded+=check;symbol.checkText+=QChar::fromLatin1(check);
  }
  expanded.prepend('*');expanded.append('*');
  for(char ch:expanded){const auto pattern=patterns[alphabet.find(ch)];for(int bit=8;bit>=0;--bit)symbol.modules.append((pattern&(1U<<bit))!=0);}
  symbol.modules.append(true);return symbol;
}
}

std::expected<Symbol,QString> encode(QStringView symbology,QStringView data,int narrow,int wide,bool checkDigit) {
  if(data.isEmpty()||data.size()>4096)return std::unexpected(QStringLiteral("Barcode data length must be 1 through 4096"));
  if(symbology==u"B8"||symbology==u"B9"||symbology==u"BU")return retail(symbology,data);
  if(symbology==u"BA")return code93(data);
  if(symbology!=u"B2"&&symbology!=u"BI")return std::unexpected(QStringLiteral("Unknown linear symbology"));
  if(!digits(data))return std::unexpected(QStringLiteral("2 of 5 requires ASCII digits"));
  if(narrow<1||narrow>10||wide<narrow||wide>30)return std::unexpected(QStringLiteral("Invalid 2 of 5 element widths"));
  Symbol result;result.text=data.toString();if(checkDigit)result.text+=mod10(data);
  if(symbology==u"B2"){
    if(result.text.size()%2)result.text.prepend(u'0');
    run(result.modules,true,narrow);run(result.modules,false,narrow);run(result.modules,true,narrow);run(result.modules,false,narrow);
    for(qsizetype i=0;i<result.text.size();i+=2){
      const auto bars=twoOfFive[result.text[i].unicode()-u'0'],spaces=twoOfFive[result.text[i+1].unicode()-u'0'];
      for(int j=0;j<5;++j){run(result.modules,true,bars[j]=='1'?wide:narrow);run(result.modules,false,spaces[j]=='1'?wide:narrow);}
    }
    run(result.modules,true,wide);run(result.modules,false,narrow);run(result.modules,true,narrow);
  }else{
    const auto pattern=[&](std::string_view bars){for(char c:bars){run(result.modules,true,c=='1'?wide:narrow);run(result.modules,false,narrow);}};
    pattern("110");for(QChar c:result.text)pattern(twoOfFive[c.unicode()-u'0']);pattern("101");
    for(int i=0;i<narrow;++i)result.modules.removeLast();
  }
  return result;
}
}
