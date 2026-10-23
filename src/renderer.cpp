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

// The only ZPL interpreter: rendering, analysis and field geometry apply
// commands here; drawing primitives live in the render_*.cpp files.
namespace QtZpl {
using namespace Rendering;
namespace {
using namespace Qt::StringLiterals;

std::optional<FieldKind> drawingKind(const Command& command,const State& state) {
  return std::visit([&](const auto& value)->std::optional<FieldKind>{
    using T=std::decay_t<decltype(value)>;
    if constexpr(std::is_same_v<T,FieldData>)return state.pendingBarcode?FieldKind::Barcode:FieldKind::Text;
    else if constexpr(std::is_same_v<T,GraphicBox>)return FieldKind::Box;
    else if constexpr(std::is_same_v<T,GraphicCircle>)return FieldKind::Circle;
    else if constexpr(std::is_same_v<T,GraphicEllipse>)return FieldKind::Ellipse;
    else if constexpr(std::is_same_v<T,GraphicDiagonal>)return FieldKind::Diagonal;
    else if constexpr(std::is_same_v<T,GraphicField>)return FieldKind::Graphic;
    else if constexpr(std::is_same_v<T,UnknownCommand>)return FieldKind::Unknown;
    else return std::nullopt;
  },command.payload);
}

FieldSettings effectiveSettings(const State& state,const Command& command,QPoint home,int top,
                                Orientation printOrientation,bool mirror) {
  FieldSettings settings;
  settings.position=state.position;settings.baseline=state.baseline;
  settings.justification=state.justification;settings.font=state.font;settings.defaultFont=state.defaultFont;
  settings.fieldDirection=state.fieldDirection;settings.barcodeDefaults=state.barcodeDefaults;
  settings.barcode=state.pendingBarcode;settings.barcodeInterpretationFont=state.barcodeInterpretationFont;
  settings.block=state.block;settings.parameter=state.parameter;settings.characterSet=state.characterSet;
  settings.hexIndicator=state.hexIndicator;
  if(const auto* field=std::get_if<FieldData>(&command.payload)){
    settings.characterSet=field->characterSet;settings.hexIndicator=field->hexIndicator;
    if(state.pendingBarcode)
      if(auto code=resolveBarcode(*state.pendingBarcode,state))settings.resolvedBarcode=std::move(*code);
  }
  settings.labelHome=home;settings.labelShift=state.labelShift;settings.labelTop=top;
  settings.reverse=state.reverse;settings.printOrientation=printOrientation;settings.mirror=mirror;
  return settings;
}

QTransform outputTransform(QSize size,Orientation orientation,bool mirror) {
  QTransform transform;
  switch(orientation){
    case Orientation::Rotated90:transform=QTransform(0,1,-1,0,size.height(),0);break;
    case Orientation::Inverted:transform=QTransform(-1,0,0,-1,size.width(),size.height());break;
    case Orientation::BottomUp:transform=QTransform(0,-1,1,0,0,size.width());break;
    default:break;
  }
  if(mirror){
    const int width=(orientation==Orientation::Rotated90||orientation==Orientation::BottomUp)?size.height():size.width();
    transform=transform*QTransform(-1,0,0,1,width,0);
  }
  return transform;
}

std::expected<QImage, RenderError> interpretLabel(const Label& label, int index, const RenderOptions& options,
  QList<Diagnostic>& diagnostics,RenderFonts& fonts,bool rasterize,QList<FieldInfo>* fields,
  QList<FieldGeometry>* geometries,int initialCharacterSet) {
  const int width=options.width>0?options.width:(label.width()>0?label.width():812);
  const int height=options.height>0?options.height:(label.height()>0?label.height():1218);
  if(rasterize&&(width<=0||height<=0||width>100000||height>100000||qint64(width)*height>64*1024*1024)) return std::unexpected(RenderError{u"invalid-size"_s,u"Label dimensions are invalid or unsafe."_s,index});
  QImage image;
  if(rasterize){
    image=QImage(width,height,QImage::Format_ARGB32_Premultiplied);
    if(image.isNull())return std::unexpected(RenderError{u"raster-allocation"_s,u"The label raster could not be allocated."_s,index});
    image.fill(options.background);
  }
  State state;state.rasterize=rasterize;state.characterSet=initialCharacterSet;
  int labelTop=0;bool mirror=false;Orientation finalOrientation=Orientation::Normal;
  for(const auto& command:label.commands()){
    if(const auto* top=std::get_if<LabelTop>(&command.payload))labelTop=top->dots;
    if(const auto* value=std::get_if<PrintMirror>(&command.payload))mirror=value->enabled;
    if(const auto* value=std::get_if<PrintOrientation>(&command.payload))finalOrientation=value->orientation;
  }
  // An explicitly requested canvas centers a narrower/wider ^PW print area.
  // Apply this before ^PO/^PM, as the Labelary canvas does.
  const int printOffset=label.width()>0?int(std::floor((qint64(width)-label.width())/2.0)):0;
  const qint64 homeX=qint64(options.ignoreLabelHome?0:label.homeX())+printOffset;
  const qint64 homeY=qint64(options.ignoreLabelHome?0:label.homeY())+labelTop;

  qsizetype segmentStart=0,segmentEnd=-1;
  SourceSpan segmentSpan;bool segmentUnknown=false;
  const auto& commands=label.commands();
  QList<qsizetype> unknownOffsets;
  if(fields||geometries){
    for(const auto& diagnostic:diagnostics)
      if(diagnostic.code==u"unsupported-command")unknownOffsets.append(diagnostic.offset);
    std::sort(unknownOffsets.begin(),unknownOffsets.end());
  }
  const QTransform transform=outputTransform(QSize(width,height),finalOrientation,mirror);
  const QRect outputCanvas=transform.mapRect(QRect(0,0,width,height));
  state.outputOrientation=finalOrientation;state.outputMirror=mirror;
  for(qsizetype commandIndex=0;commandIndex<commands.size();++commandIndex) {
    if(options.stopToken.stop_requested())return std::unexpected(RenderError{u"operation-cancelled"_s,u"The operation was cancelled."_s,index});
    const auto& command=commands[commandIndex];
    state.reverse=state.labelReverse||state.fieldReverse;
    const auto kind=drawingKind(command,state);
    std::optional<FieldInfo> field;
    State::Observation observation;
    state.observation=geometries&&kind?&observation:nullptr;
    if((fields||geometries)&&kind){
      if(segmentEnd<commandIndex){
        segmentStart=commandIndex;
        while(segmentStart>0&&!std::holds_alternative<FieldSeparator>(commands[segmentStart-1].payload)
          &&!std::holds_alternative<FormatStart>(commands[segmentStart-1].payload))--segmentStart;
        segmentEnd=commandIndex;segmentUnknown=false;
        while(segmentEnd+1<commands.size()&&!std::holds_alternative<FieldSeparator>(commands[segmentEnd].payload)
          &&!std::holds_alternative<FormatEnd>(commands[segmentEnd+1].payload))++segmentEnd;
        // Omitted unknown commands still belong to this source segment. Start
        // at the preceding field/format boundary, not the first saved command.
        const auto boundary=segmentStart>0?commands[segmentStart-1].sourceSpan:label.sourceSpan();
        const qsizetype sourceStart=segmentStart>0?boundary.start+boundary.length:boundary.start;
        const auto end=commands[segmentEnd].sourceSpan;
        segmentSpan={sourceStart,end.start+end.length-sourceStart};
        for(qsizetype i=segmentStart;i<=segmentEnd;++i)
          segmentUnknown|=std::holds_alternative<UnknownCommand>(commands[i].payload);
        if(!segmentUnknown){
          const auto unknown=std::lower_bound(unknownOffsets.cbegin(),unknownOffsets.cend(),segmentSpan.start);
          segmentUnknown=unknown!=unknownOffsets.cend()&&*unknown<segmentSpan.start+segmentSpan.length;
        }
      }
      field=FieldInfo{commandOffset(command),index,commandIndex,segmentSpan,command.sourceSpan,*kind,
        effectiveSettings(state,command,QPoint(label.homeX(),label.homeY()),labelTop,finalOrientation,mirror),segmentUnknown};
      if(fields)fields->append(*field);
    }
    const qsizetype firstDiagnostic=diagnostics.size();
    std::visit([&](const auto& value){
      using T=std::decay_t<decltype(value)>;
      if constexpr(std::is_same_v<T,FieldOrigin>){state.position=printerPosition(qint64(value.x)+state.labelShift+homeX,qint64(value.y)+homeY);state.baseline=false;state.justification=value.useDefaultJustification?state.defaultJustification:value.justification;}
      else if constexpr(std::is_same_v<T,FieldTypeset>){state.position=printerPosition(value.usePreviousX?state.nextTypeset.x():qint64(value.x)+state.labelShift+homeX,value.usePreviousY?state.nextTypeset.y():qint64(value.y)+homeY);state.baseline=true;state.justification=value.useDefaultJustification?state.defaultJustification:value.justification;}
      else if constexpr(std::is_same_v<T,ScalableFont>) {state.font=value;state.barcodeInterpretationFont=value;}
      else if constexpr(std::is_same_v<T,FieldSeparator>) {state.font=state.defaultFont;state.barcodeInterpretationFont.reset();state.fieldReverse=false;state.parameter={};state.pendingBarcode.reset();state.block.reset();state.hexIndicator={};}
      else if constexpr(std::is_same_v<T,ChangeFont>){state.defaultFont={value.font,Orientation::Normal,value.height,value.width,true};state.font=state.defaultFont;}
      else if constexpr(std::is_same_v<T,FieldDirection>){
        // The later of ^FW and an explicit ^A rotation wins for the current font.
        state.fieldDirection=value.orientation;state.defaultJustification=value.justification;
        state.font.orientationOmitted=true;
      }
      else if constexpr(std::is_same_v<T,FieldReverse>) state.fieldReverse=true;
      else if constexpr(std::is_same_v<T,LabelReverse>) state.labelReverse=value.enabled;
      else if constexpr(std::is_same_v<T,FieldParameter>) state.parameter=value;
      else if constexpr(std::is_same_v<T,FieldBlock>) state.block=value;
      else if constexpr(std::is_same_v<T,FieldEncoding>) { /* Accepted compatibility command; no raster state change. */ }
      else if constexpr(std::is_same_v<T,CharacterSet>) state.characterSet=value.id;
      else if constexpr(std::is_same_v<T,FieldHex>) state.hexIndicator=value.indicator;
      else if constexpr(std::is_same_v<T,BarcodeDefault>){
        // Labelary raises a ^BY height below one dot to its minimum of 1.
        state.barcodeDefaults=value;state.barcodeDefaults.height=std::max(1,value.height);
      }
      else if constexpr(std::is_same_v<T,LabelShift>) state.labelShift=value.dots;
      else if constexpr(std::is_same_v<T,PrintMode>) { /* Print mode does not change local raster geometry. */ }
      else if constexpr(std::is_same_v<T,PrinterSetting>) { /* Printer configuration never changes the label raster. */ }
      else if constexpr(std::is_same_v<T,PrintOrientation>) state.printOrientation=value.orientation;
      else if constexpr(std::is_same_v<T,Barcode>){
        state.pendingBarcode=value;state.barcodeOffset=commandOffset(command);state.barcodeSource=normalizedCommand(command);
        state.barcodeDirection=state.fieldDirection;
        // QR magnification also changes the following barcode module width,
        // even if this ^BQ has no field data. ^BY subsequently overrides it.
        if(value.symbology==u"BQ")state.barcodeDefaults.moduleWidth=qrMagnification(value);
      }
      else if constexpr(std::is_same_v<T,FieldData>){
        if(state.pendingBarcode){
          if(!rasterize){state.pendingBarcode.reset();state.reverse=false;state.block.reset();return;}
          const auto firstDiagnostic=diagnostics.size();
          drawBarcode(image,state,*state.pendingBarcode,value,options,diagnostics,fonts);
          for(qsizetype i=firstDiagnostic;i<diagnostics.size();++i){
            if(diagnostics[i].offset<0)diagnostics[i].offset=state.barcodeOffset;
            diagnostics[i].command=state.barcodeSource;
          }
        }else{
          const auto firstDiagnostic=diagnostics.size();
          // ^A@ names a printer font that is not available locally.
          if(rasterize&&state.font.external)diagnostics.append({Severity::Warning,u"external-font-substituted"_s,
            u"The ^A@ printer font is not available; the field is rendered with the embedded Font 0."_s,commandOffset(command)});
          drawText(image,state,value.data,options,fonts,diagnostics,commandOffset(command));
          for(qsizetype i=firstDiagnostic;i<diagnostics.size();++i)diagnostics[i].command=normalizedCommand(command);
        }
      }
      else if constexpr(std::is_same_v<T,GraphicBox>){
        if(value.width<0||value.height<0||value.width>32000||value.height>32000||value.thickness>32000){diagnostics.append({Severity::Error,u"graphic-dimensions"_s,u"Graphic dimensions must be between 0 and 32000 dots."_s,commandOffset(command),normalizedCommand(command)});state.reverse=false;return;}
        const int boxWidth=value.width>0?value.width:value.thickness;
        const int boxHeight=value.height>0?value.height:value.thickness;
        const QPoint origin=state.position-QPoint(state.justification==Justification::Right?boxWidth:0,state.baseline?boxHeight:0);
        const auto fillRectangle=QRect(origin,QSize(boxWidth,boxHeight));
        observeLogical(state,fillRectangle);
        if(state.reverse||commandColor(value.color,options).alpha()>0)observePaint(state,fillRectangle);
        if(!rasterize){state.reverse=false;return;}
        const bool filled=qint64(value.thickness)*2>=std::min(boxWidth,boxHeight);
        QPainterPath path;path.setFillRule(Qt::OddEvenFill);path.addRect(QRectF(fillRectangle));
        if(!filled){const int inset=std::min({value.thickness,boxWidth/2,boxHeight/2});path.addRect(QRectF(fillRectangle.adjusted(inset,inset,-inset,-inset)));}
        QPainter p(&image);p.setRenderHint(QPainter::Antialiasing,false);p.setPen(Qt::NoPen);p.setBrush(state.reverse?Qt::white:commandColor(value.color,options));
        if(state.reverse)p.setCompositionMode(QPainter::CompositionMode_Difference);p.drawPath(path);
        state.reverse=false;
      }
      else if constexpr(std::is_same_v<T,GraphicCircle>||std::is_same_v<T,GraphicEllipse>){
        const int width=[](const auto& graphic){if constexpr(std::is_same_v<T,GraphicCircle>)return graphic.diameter;else return graphic.width;}(value);
        const int height=[](const auto& graphic){if constexpr(std::is_same_v<T,GraphicCircle>)return graphic.diameter;else return graphic.height;}(value);
        if(width<0||height<0||width>32000||height>32000||value.thickness>32000){diagnostics.append({Severity::Error,u"graphic-dimensions"_s,u"Graphic dimensions must be between 0 and 32000 dots."_s,commandOffset(command),normalizedCommand(command)});state.reverse=false;return;}
        const QPoint origin=GraphicGeometry::anchoredOrigin(state.position,width,height,state.justification==Justification::Right,state.baseline);
        observeLogical(state,QRect(origin,QSize(width,height)));
        if(state.observation&&width>0&&height>0&&(state.reverse||commandColor(value.color,options).alpha()>0)){
          const auto outer=GraphicGeometry::ellipse(origin,width,height,false);
          QRect paint;
          for(int y=origin.y();y<qint64(origin.y())+height;++y){
            const auto [left,right]=GraphicGeometry::span(outer,y);
            if(right>left)paint|=QRect(left,y,right-left,1);
          }
          observePaint(state,paint);
        }
        if(!rasterize){state.reverse=false;return;}
        QPainter p(&image);p.setRenderHint(QPainter::Antialiasing,false);p.setPen(Qt::NoPen);p.setBrush(state.reverse?Qt::white:commandColor(value.color,options));
        if(state.reverse)p.setCompositionMode(QPainter::CompositionMode_Difference);
        GraphicGeometry::drawEllipse(p,origin,width,height,value.thickness,image.size());state.reverse=false;
      }
      else if constexpr(std::is_same_v<T,GraphicDiagonal>){
        if(value.width<0||value.height<0||value.width>32000||value.height>32000||value.thickness>32000){diagnostics.append({Severity::Error,u"graphic-dimensions"_s,u"Graphic dimensions must be between 0 and 32000 dots."_s,commandOffset(command),normalizedCommand(command)});state.reverse=false;return;}
        const QPoint origin=GraphicGeometry::anchoredOrigin(state.position,value.width,value.height,state.justification==Justification::Right,state.baseline);
        observeLogical(state,QRect(origin,QSize(value.width,value.height)));
        if(value.width>0&&value.height>0&&value.thickness>0&&(state.reverse||commandColor(value.color,options).alpha()>0)){
          const qint64 step=qint64(value.width)*1024/value.height;
          const qint64 last=step*(value.height-1);
          const int firstX=value.orientation==u'L'?0:int((qint64(value.width)*1024-last+1023)/1024);
          const int lastX=value.orientation==u'L'?int((last+1023)/1024):value.width;
          observePaint(state,QRect(origin+QPoint(firstX,0),QSize(lastX-firstX+value.thickness,value.height)));
        }
        if(!rasterize){state.reverse=false;return;}
        QPainter p(&image);p.setPen(Qt::NoPen);p.setBrush(state.reverse?Qt::white:commandColor(value.color,options));
        if(state.reverse)p.setCompositionMode(QPainter::CompositionMode_Difference);
        GraphicGeometry::drawDiagonal(p,origin,value.width,value.height,value.thickness,value.orientation==u'L',image.size());
        state.reverse=false;
      }
      else if constexpr(std::is_same_v<T,GraphicField>) {
        if(!rasterize)return;
        const auto decoded=decodeGraphic(value,{options.maxGraphicBytes,options.stopToken});
        if(!decoded){
          diagnostics.append({Severity::Error,decoded.error().code,decoded.error().message,commandOffset(command),normalizedCommand(command)});return;
        }
        QPoint position=state.position;
        if(state.baseline)position=printerPosition(position.x(),qint64(position.y())-decoded->size.height());
        if(state.justification==Justification::Right)position=printerPosition(qint64(position.x())-decoded->size.width(),position.y());
        if(state.reverse){
          QImage mask(image.size(),QImage::Format_ARGB32_Premultiplied);mask.fill(Qt::transparent);
          auto ink=options;ink.background=Qt::transparent;ink.foreground=Qt::white;
          drawGraphicField(mask,position,*decoded,ink,state);
          QPainter painter(&image);painter.setCompositionMode(QPainter::CompositionMode_Difference);painter.drawImage(0,0,mask);
        }else drawGraphicField(image,position,*decoded,options,state);
      }
    },command.payload);
    if(const auto fontError=fonts.takeFont0Error();!fontError.isEmpty())
      diagnostics.append({Severity::Error,u"font0-outline"_s,fontError,commandOffset(command),normalizedCommand(command)});
    if(options.stopToken.stop_requested())return std::unexpected(RenderError{u"operation-cancelled"_s,u"The operation was cancelled."_s,index});
    bool error=false,unsupported=kind==FieldKind::Unknown;
    for(qsizetype i=firstDiagnostic;i<diagnostics.size();++i){
      auto& diagnostic=diagnostics[i];diagnostic.labelIndex=index;
      if(kind)diagnostic.fieldId=commandOffset(command);
      if(diagnostic.offset<0)diagnostic.offset=commandOffset(command);
      if(!diagnostic.sourceSpan){
        const auto source=std::lower_bound(commands.cbegin(),commands.cend(),diagnostic.offset,
          [](const Command& candidate,qsizetype offset){return commandOffset(candidate)<offset;});
        if(source!=commands.cend()&&commandOffset(*source)==diagnostic.offset){
          diagnostic.sourceSpan=source->sourceSpan;diagnostic.command=normalizedCommand(*source);
        }
      }
      if(diagnostic.command.isEmpty())diagnostic.command=normalizedCommand(command);
      error|=diagnostic.severity==Severity::Error;
      unsupported|=diagnostic.code==u"barcode-render-pending";
    }
    if(diagnostics.size()>options.maxDiagnostics)
      return std::unexpected(RenderError{u"render-diagnostic-limit"_s,u"The diagnostic budget was exceeded."_s,index});
    if(geometries&&field){
      FieldGeometry geometry;geometry.field=std::move(*field);
      geometry.logicalBounds=transform.mapRect(observation.logicalBounds);
      geometry.paintBounds=transform.mapRect(observation.paintBounds);
      geometry.clippedBounds=geometry.paintBounds.intersected(outputCanvas);
      geometry.anchor=transform.map(geometry.field.settings.position);
      geometry.baseline=geometry.field.settings.baseline;geometry.labelTransform=transform;
      geometry.status=error?FieldStatus::Error:unsupported?FieldStatus::Unsupported:
        geometry.paintBounds.isEmpty()?FieldStatus::Empty:geometry.clippedBounds.isEmpty()?FieldStatus::Clipped:FieldStatus::Drawn;
      geometries->append(std::move(geometry));
    }
  }
  if(!rasterize)return QImage{};
  image=rotateImage(image,state.printOrientation);
  if(mirror)image.flip(Qt::Horizontal);
  return image;
}

} // namespace

namespace {
int nextCharacterSet(const Label& label,int previous) {
  for(const auto& command:label.commands())
    if(const auto* value=std::get_if<CharacterSet>(&command.payload))previous=value->id;
  return previous;
}

int initialCharacterSet(const Document& document,int labelIndex) {
  int result=0;
  for(int i=0;i<labelIndex;++i)result=nextCharacterSet(document.labels()[i],result);
  return result;
}

std::expected<void,RenderError> validateOperation(const Document& document,const RenderOptions& options,
  std::optional<int> selected,bool rasterize) {
  if(options.stopToken.stop_requested())
    return std::unexpected(RenderError{u"operation-cancelled"_s,u"The operation was cancelled."_s,selected.value_or(-1)});
  if(options.dpi!=203&&options.dpi!=300&&options.dpi!=600)
    return std::unexpected(RenderError{u"invalid-dpi"_s,u"DPI must be 203, 300, or 600."_s,-1});
  if(selected&&(*selected<0||*selected>=document.labels().size()))
    return std::unexpected(RenderError{u"invalid-label-index"_s,u"The label index is outside the document."_s,*selected});
  if(options.width<0||options.height<0||options.maxTotalPixels<0||options.maxGraphicBytes<0||options.maxDiagnostics<0)
    return std::unexpected(RenderError{u"invalid-render-options"_s,u"Render limits and dimensions must not be negative."_s,-1});
  if(document.diagnostics().size()>options.maxDiagnostics)
    return std::unexpected(RenderError{u"render-diagnostic-limit"_s,u"The diagnostic budget was exceeded."_s,-1});
  qint64 pixels=0,graphicBytes=0;
  const qsizetype first=selected.value_or(0),last=selected?*selected+1:document.labels().size();
  for(qsizetype i=first;i<last;++i){
    const auto& label=document.labels()[i];
    if(rasterize){
      const qint64 width=options.width>0?options.width:label.width()>0?label.width():812;
      const qint64 height=options.height>0?options.height:label.height()>0?label.height():1218;
      if(width>100000||height>100000||width*height>64*1024*1024)
        return std::unexpected(RenderError{u"invalid-size"_s,u"Label dimensions are invalid or unsafe."_s,int(i)});
      if(width*height>options.maxTotalPixels-pixels)
        return std::unexpected(RenderError{u"render-pixel-limit"_s,u"The total output pixel budget was exceeded."_s,int(i)});
      pixels+=width*height;
      for(const auto& command:label.commands()){
        if(options.stopToken.stop_requested())
          return std::unexpected(RenderError{u"operation-cancelled"_s,u"The operation was cancelled."_s,int(i)});
        if(const auto* graphic=std::get_if<GraphicField>(&command.payload)){
          const qint64 bytes=graphic->bytesPerRow>0&&graphic->totalBytes>0
            ?((qint64(graphic->totalBytes)+graphic->bytesPerRow-1)/graphic->bytesPerRow)*graphic->bytesPerRow:0;
          if(bytes>options.maxGraphicBytes-graphicBytes)
            return std::unexpected(RenderError{u"render-graphic-limit"_s,u"The total decoded graphic budget was exceeded."_s,int(i)});
          graphicBytes+=bytes;
        }
      }
    }
  }
  return {};
}
}

std::expected<RenderResult, RenderError> render(const Document& document, const RenderOptions& options) {
  if(auto valid=validateOperation(document,options,std::nullopt,true);!valid)return std::unexpected(valid.error());
  RenderResult result;result.diagnostics=document.diagnostics();result.labels.reserve(document.labels().size());
  RenderFonts fonts;int characterSet=0;
  for(int i=0;i<document.labels().size();++i){
    auto image=interpretLabel(document.labels()[i],i,options,result.diagnostics,fonts,true,nullptr,
      options.collectFieldGeometry?&result.fields:nullptr,characterSet);
    if(!image)return std::unexpected(image.error());
    result.labels.append(std::move(*image));
    characterSet=nextCharacterSet(document.labels()[i],characterSet);
  }
  return result;
}

std::expected<LabelRenderResult,RenderError> renderLabel(const Document& document,int labelIndex,const RenderOptions& options) {
  if(auto valid=validateOperation(document,options,labelIndex,true);!valid)return std::unexpected(valid.error());
  LabelRenderResult result;result.labelIndex=labelIndex;result.diagnostics=document.diagnostics();
  RenderFonts fonts;
  auto image=interpretLabel(document.labels()[labelIndex],labelIndex,options,result.diagnostics,fonts,true,nullptr,
    options.collectFieldGeometry?&result.fields:nullptr,initialCharacterSet(document,labelIndex));
  if(!image)return std::unexpected(image.error());
  result.image=std::move(*image);
  return result;
}

std::expected<AnalysisResult,RenderError> analyze(const Document& document,const RenderOptions& options) {
  if(auto valid=validateOperation(document,options,std::nullopt,false);!valid)return std::unexpected(valid.error());
  AnalysisResult result;result.diagnostics=document.diagnostics();RenderFonts fonts;int characterSet=0;
  for(int i=0;i<document.labels().size();++i){
    auto analyzed=interpretLabel(document.labels()[i],i,options,result.diagnostics,fonts,false,&result.fields,nullptr,characterSet);
    if(!analyzed)return std::unexpected(analyzed.error());
    characterSet=nextCharacterSet(document.labels()[i],characterSet);
  }
  return result;
}

std::expected<AnalysisResult,RenderError> analyzeLabel(const Document& document,int labelIndex,const RenderOptions& options) {
  if(auto valid=validateOperation(document,options,labelIndex,false);!valid)return std::unexpected(valid.error());
  AnalysisResult result;result.diagnostics=document.diagnostics();RenderFonts fonts;
  auto analyzed=interpretLabel(document.labels()[labelIndex],labelIndex,options,result.diagnostics,fonts,false,&result.fields,nullptr,initialCharacterSet(document,labelIndex));
  if(!analyzed)return std::unexpected(analyzed.error());
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
