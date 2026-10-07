#pragma once

#include <QtCore/QPoint>
#include <QtCore/QVector>
#include <QtGui/QPainter>
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>

namespace QtZpl::GraphicGeometry {

// Printer shapes use integer scan lines. The outline is a cubic approximation
// of an ellipse, with fixed-point polynomial coefficients; evaluating the
// analytic ellipse instead changes boundary dots. This implementation evaluates
// the polynomial directly rather than relying on a platform paint engine.
struct FixedPoint { qint64 x=0,y=0; };
using Curve=std::array<std::array<float,2>,4>;

inline QPoint anchoredOrigin(QPoint position,int width,int height,bool right,bool baseline) {
  const auto coordinate=[](qint64 n){return int(std::clamp(n,qint64(std::numeric_limits<int>::min()),qint64(std::numeric_limits<int>::max())));};
  return {coordinate(qint64(position.x())-(right?width:0)),coordinate(qint64(position.y())-(baseline?height:0))};
}

inline void appendCurve(QVector<FixedPoint>& polygon,const Curve& curve) {
  const auto extent=[&](int axis) {
    float low=curve[0][axis],high=low;
    for(const auto& point:curve){low=std::min(low,point[axis]);high=std::max(high,point[axis]);}
    return high-low;
  };
  if(extent(0)>256||extent(1)>256){
    const auto midpoint=[](const auto& a,const auto& b){return std::array<float,2>{(a[0]+b[0])/2,(a[1]+b[1])/2};};
    const auto ab=midpoint(curve[0],curve[1]),bc=midpoint(curve[1],curve[2]),cd=midpoint(curve[2],curve[3]);
    const auto abc=midpoint(ab,bc),bcd=midpoint(bc,cd),center=midpoint(abc,bcd);
    appendCurve(polygon,{curve[0],ab,abc,center});appendCurve(polygon,{center,bcd,cd,curve[3]});
    return;
  }
  struct Polynomial {qint64 a,b,c,start;};
  std::array<Polynomial,2> coefficients;
  for(int axis=0;axis<2;++axis){
    const float p0=curve[0][axis],p1=curve[1][axis],p2=curve[2][axis],p3=curve[3][axis];
    coefficients[axis]={qint64((-p0+3*p1-3*p2+p3)*128),qint64((3*p0-6*p1+3*p2)*2048),
                        qint64((-3*p0+3*p1)*8192),qint64(p0*1024)};
  }
  int subdivisions=1;
  for(const auto& p:coefficients)
    while(std::max(std::abs(6*p.a+p.b),std::abs(48*p.a+p.b))>262144LL*subdivisions*subdivisions)subdivisions*=2;
  const qint64 divisor=128LL*subdivisions*subdivisions*subdivisions;
  const auto floorDivide=[](qint64 n,qint64 d){return n/d-(n<0&&n%d!=0);};
  for(qint64 i=0;i<8*subdivisions;++i){
    const auto evaluate=[&](const Polynomial& p){
      return p.start+floorDivide(2*p.a*i*i*i+p.b*i*i*subdivisions+(2*p.c-(p.b&1))*i*subdivisions*subdivisions,divisor);
    };
    polygon.append({evaluate(coefficients[0]),evaluate(coefficients[1])});
  }
}

inline QVector<FixedPoint> ellipse(QPoint origin,int width,int height,bool clockwise=true) {
  constexpr double k=.5522847498307933;
  constexpr std::array<std::array<std::array<double,2>,4>,4> curves{{
    {{{1,.5},{1,(1+k)/2},{(1+k)/2,1},{.5,1}}},
    {{{.5,1},{(1-k)/2,1},{0,(1+k)/2},{0,.5}}},
    {{{0,.5},{0,(1-k)/2},{(1-k)/2,0},{.5,0}}},
    {{{.5,0},{(1+k)/2,0},{1,(1-k)/2},{1,.5}}}
  }};
  QVector<FixedPoint> polygon;polygon.reserve(64);
  for(int quadrant=0;quadrant<4;++quadrant){
    const auto& normalized=curves[clockwise?quadrant:3-quadrant];
    Curve curve;
    for(int i=0;i<4;++i){
      const auto& point=normalized[clockwise?i:3-i];
      curve[i]={float(origin.x()+point[0]*width),float(origin.y()+point[1]*height)};
    }
    appendCurve(polygon,curve);
  }
  return polygon;
}

inline QPair<int,int> span(const QVector<FixedPoint>& polygon,int y) {
  const qint64 scan=qint64(y)*1024;
  int left=std::numeric_limits<int>::max(),right=std::numeric_limits<int>::min();
  const auto ceiling=[](qint64 n,qint64 d){return n/d+(n>0&&n%d!=0);};
  for(qsizetype i=0;i<polygon.size();++i){
    auto a=polygon[i],b=polygon[(i+1)%polygon.size()];
    if(a.y>b.y)std::swap(a,b);
    if(a.y==b.y||scan<a.y||scan>=b.y)continue;
    const qint64 denominator=b.y-a.y,delta=b.x-a.x;
    const qint64 firstScan=ceiling(a.y,1024);
    const qint64 initial=a.x+delta*(firstScan*1024-a.y)/denominator;
    const qint64 step=delta*1024/denominator;
    const int x=int(ceiling(initial+(y-firstScan)*step,1024));
    left=std::min(left,x);right=std::max(right,x);
  }
  return left<right?QPair<int,int>{left,right}:QPair<int,int>{0,0};
}

inline void drawEllipse(QPainter& painter,QPoint origin,int width,int height,int thickness,QSize imageSize) {
  if(width<=0||height<=0)return;
  if(origin.x()>=imageSize.width()||origin.y()>=imageSize.height()||qint64(origin.x())+width<=0||qint64(origin.y())+height<=0)return;
  // The ring is a single area: its outer boundary runs counterclockwise and
  // the hole clockwise. Reversing a quantized cubic changes boundary dots.
  const auto outer=ellipse(origin,width,height,false);
  const bool hollow=qint64(thickness)*2<std::min(width,height);
  const auto inner=hollow?ellipse(origin+QPoint(thickness,thickness),width-2*thickness,height-2*thickness):QVector<FixedPoint>{};
  for(int y=std::max(0,origin.y());y<std::min(qint64(imageSize.height()),qint64(origin.y())+height);++y){
    const auto [left,right]=span(outer,y);if(right<=left)continue;
    const auto [holeLeft,holeRight]=span(inner,y);
    if(holeRight<=holeLeft)painter.drawRect(left,y,right-left,1);
    else {if(holeLeft>left)painter.drawRect(left,y,holeLeft-left,1);if(right>holeRight)painter.drawRect(holeRight,y,right-holeRight,1);}
  }
}

inline void drawDiagonal(QPainter& painter,QPoint origin,int width,int height,int thickness,bool ascending,QSize imageSize) {
  if(width<=0||height<=0)return;
  if(origin.x()>=imageSize.width()||origin.y()>=imageSize.height()||qint64(origin.x())+width+thickness<=0||qint64(origin.y())+height<=0)return;
  const qint64 step=qint64(width)*1024/height;
  for(int y=int(std::max(0LL,-qint64(origin.y())));y<std::min(qint64(height),qint64(imageSize.height())-origin.y());++y){
    const qint64 position=ascending?step*y:qint64(width)*1024-step*y;
    const int x=int((position+1023)/1024);
    painter.drawRect(origin.x()+x,origin.y()+y,thickness,1);
  }
}

} // namespace QtZpl::GraphicGeometry
