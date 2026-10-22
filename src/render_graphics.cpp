// Printer graphics (^GF) composition.
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

void drawGraphicField(QImage& image,const QPoint& pos,const DecodedGraphic& decoded,const RenderOptions& options,
                      const State& state) {
  {
    const QByteArrayView bytes{decoded.bytes};
    observeLogical(state,QRect(pos,decoded.size));
    if(state.observation&&options.foreground.alpha()>0){
      QRect paint;
      for(int y=0;y<decoded.size.height();++y){
        int first=-1,last=-1;
        for(int x=0;x<decoded.bytesPerRow;++x){
          const auto byte=static_cast<unsigned char>(bytes[qsizetype(y)*decoded.bytesPerRow+x]);
          if(byte){if(first<0)first=x*8+std::countl_zero(byte);last=x*8+7-std::countr_zero(byte);}
        }
        if(first>=0)paint|=QRect(pos+QPoint(first,y),QSize(last-first+1,1));
      }
      observePaint(state,paint);
    }
    // SourceOver with fully opaque ink replaces the destination exactly.
    // Clip in wide arithmetic before requesting scanlines; input validation
    // above still processes the entire graphic, even when it is off-label.
    // QColor::alpha() rounds 16-bit alpha, so it cannot decide opacity here.
    if(options.foreground.rgba64().alpha()==65535){
      if(bytes.isEmpty())return;
      const qsizetype rowBytes=decoded.bytesPerRow;
      const qint64 firstX=std::max<qint64>(0,-static_cast<qint64>(pos.x()));
      const qint64 lastX=std::min<qint64>(static_cast<qint64>(rowBytes)*8,
        static_cast<qint64>(image.width())-pos.x());
      const qint64 firstY=std::max<qint64>(0,-static_cast<qint64>(pos.y()));
      const qint64 lastY=std::min<qint64>((bytes.size()-1)/rowBytes+1,
        static_cast<qint64>(image.height())-pos.y());
      if(firstX>=lastX||firstY>=lastY)return;
      const QRgb ink=options.foreground.rgba();
      for(qint64 y=firstY;y<lastY;++y){
        const qsizetype rowStart=static_cast<qsizetype>(y*rowBytes);
        const qint64 end=std::min(lastX,static_cast<qint64>(bytes.size()-rowStart)*8);
        auto* pixels=reinterpret_cast<QRgb*>(image.scanLine(static_cast<int>(pos.y()+y)));
        for(qint64 x=firstX;x<end;++x)
          if(static_cast<unsigned char>(bytes[rowStart+static_cast<qsizetype>(x/8)])&(1U<<(7-x%8)))
            pixels[static_cast<qsizetype>(pos.x()+x)]=ink;
      }
      return;
    }
    qint64 x=0,y=0;
    const qint64 rowWidth=decoded.size.width();
    QPainter painter(&image);painter.setPen(options.foreground);
    for(const unsigned char byte:bytes){
      for(int bit=7;bit>=0;--bit){
        if(byte&(1U<<bit)){
          const qint64 targetX=pos.x()+x,targetY=pos.y()+y;
          if(targetX>=0&&targetX<image.width()&&targetY>=0&&targetY<image.height())
            painter.drawPoint(static_cast<int>(targetX),static_cast<int>(targetY));
        }
        if(++x>=rowWidth){x=0;++y;}
      }
    }
    return;
  }
}

} // namespace QtZpl::Rendering
