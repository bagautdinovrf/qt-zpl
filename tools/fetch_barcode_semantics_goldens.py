"""Manual maintainer tool. Normal builds/tests never access Labelary.

Fetches bitonal Labelary references for barcode parameter defaults, Y/N case
and ^FW inheritance (Designer stage 2, discrepancies D1-D6), and for the
follow-up cases: interpretation lines at ^BY1..10, EAN-13 digits, Aztec sizes
and Codabar start/stop parameters.

--refresh-ean13 also re-fetches the older top-level EAN-13 references whose
digits below the bars came from an earlier Labelary release (Font A at ^BY3+),
with their own ZPL and size.
"""
from pathlib import Path
import json
import math
import struct
import sys
import time
import urllib.request
import urllib.error

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'tests/golden/barcode-semantics'
OUT.mkdir(exist_ok=True)
cases={}
# D1/D6: empty height parameter while ^BY sets height 0.
for name,command,data in [('bc','^BCN,,N,N','12345'),('be','^BEN,,N,N','123456789012'),
                          ('b8','^B8N,,N,N','1234567'),('bu','^BUN,,N,N','03600029145'),
                          ('b9','^B9N,,N,N','0425261'),('ba','^BAN,,N,N','ABC123'),
                          ('b2','^B2N,,N,N','12345678'),('bi','^BIN,,N,N','12345678'),
                          ('b3','^B3N,N,,N,N','ABC'),('bk','^BKN,N,,N,N','A123A')]:
    cases[f'{name}-by0-height']=f'^BY2,3,0^FO40,40{command}^FD{data}^FS'
    cases[f'{name}-by-height']=f'^BY2,3,30^FO40,40{command}^FD{data}^FS'
# D3: MicroPDF417 row height omitted.
cases['bf-row-height-by30']='^BY2,3,30^FO40,40^BFN,,3^FDABC123^FS'
cases['bf-row-height-by0']='^BY2,3,0^FO40,40^BFN,,3^FDABC123^FS'
cases['bf-row-height-by10']='^BY2^FO40,40^BFN,,3^FDABC123^FS'
cases['bf-row-height-by100']='^BY2,3,100^FO40,40^BFN,,3^FDABC123^FS'
cases['bf-row-height-by200']='^BY2,3,200^FO40,40^BFN,,3^FDABC123^FS'
# ^BQ moves its ^FO anchor by the ^BY height; ^BY height 0 is clamped.
for height in ['0','1','']:
    suffix=height or 'default'
    by=f'^BY2,3,{height}' if height else '^BY2'
    cases[f'bq-by{suffix}-offset']=f'{by}^FO40,40^BQN,2,3^FDQA,ABC123^FS'
# D4: lower-case Y/N flags.
cases['bc-lower']='^BY2^FO40,40^BCN,60,y,y^FD12345^FS'
cases['be-lower']='^BY2^FO40,60^BEN,60,y,y^FD123456789012^FS'
cases['b8-lower-caption']='^BY2^FO40,60^B8N,60,y,y^FD1234567^FS'
cases['b8-lower-no-caption']='^BY2^FO40,60^B8N,60,n^FD1234567^FS'
cases['bu-lower-check']='^BY2^FO60,60^BUN,60,Y,N,n^FD03600029145^FS'
cases['b9-lower-check']='^BY2^FO60,60^B9N,60,Y,N,n^FD0425261^FS'
cases['ba-lower-check']='^BY2^FO40,60^BAN,60,Y,N,y^FDABC123^FS'
cases['b2-lower-check']='^BY2^FO40,60^B2N,60,Y,N,y^FD1234567^FS'
cases['b7-lower-truncate']='^BY2^FO40,40^B7N,6,2,3,,y^FDABC123^FS'
cases['bo-lower-flags']='^FO40,40^BON,4,n,0,n^FDABC123^FS'
# Upper-case twins separate Y/N case handling from caption layout.
cases['be-upper']='^BY2^FO40,60^BEN,60,Y,Y^FD123456789012^FS'
cases['b8-upper-caption']='^BY2^FO40,60^B8N,60,Y,Y^FD1234567^FS'
cases['bu-upper-check']='^BY2^FO60,60^BUN,60,Y,N,N^FD03600029145^FS'
cases['b9-upper-check']='^BY2^FO60,60^B9N,60,Y,N,N^FD0425261^FS'
cases['ba-upper-check']='^BY2^FO40,60^BAN,60,Y,N,Y^FDABC123^FS'
cases['b2-upper-check']='^BY2^FO40,60^B2N,60,Y,N,Y^FD1234567^FS'
cases['bo-upper-flags']='^FO40,40^BON,4,N,0,N^FDABC123^FS'
# D5: ^FW and omitted or explicit barcode/text orientation.
cases['fw-r-bc-omitted']='^FWR^BY2^FO200,40^BC,60,N,N^FD12345^FS'
cases['fw-r-bc-explicit-n']='^FWR^BY2^FO200,40^BCN,60,N,N^FD12345^FS'
cases['fw-r-bc-default']='^FWR^BY2^FO200,40^BC^FD12345^FS'
cases['fw-b-b8-omitted']='^FWB^BY2^FO200,40^B8,60,N,N^FD1234567^FS'
cases['fw-i-b3-omitted']='^FWI^BY2^FO200,40^B3,N,60,N,N^FDABC^FS'
cases['fw-r-bx-omitted']='^FWR^FO200,40^BX,4,200^FDABC123^FS'
cases['fw-r-b7-omitted']='^FWR^BY2^FO200,40^B7,4,2,3^FDABC123^FS'
cases['fw-r-bq']='^FWR^FO200,40^BQ,2,3^FDQA,ABC123^FS'
cases['bq-n']='^FO200,40^BQN,2,3^FDQA,ABC123^FS'
for rotation in 'RIB':
    cases[f'bq-{rotation.lower()}']=f'^FO200,40^BQ{rotation},2,3^FDQA,ABC123^FS'
for rotation in 'IB':
    cases[f'fw-{rotation.lower()}-bq']=f'^FW{rotation}^FO200,40^BQ,2,3^FDQA,ABC123^FS'
cases['fw-r-bq-explicit-n']='^FWR^FO200,40^BQN,2,3^FDQA,ABC123^FS'
cases['fw-r-bd']='^FWR^FO200,40^BD4,1,1^FH_^FDHELLO WORLD 123456789^FS'
cases['bd-n']='^FO200,40^BD4,1,1^FH_^FDHELLO WORLD 123456789^FS'
cases['fw-r-bx-explicit-n']='^FWR^FO200,40^BXN,4,200^FDABC123^FS'
cases['bx-n']='^FO200,40^BXN,4,200^FDABC123^FS'
cases['bx-r']='^FO200,40^BXR,4,200^FDABC123^FS'
cases['fw-r-text-omitted']='^FWR^FO200,40^A0,40,40^FDABC^FS'
cases['fw-r-text-explicit-n']='^FWR^FO200,40^A0N,40,40^FDABC^FS'
cases['fw-r-text-cf']='^FWR^CF0,40,40^FO200,40^FDABC^FS'
# The later of an explicit rotation and ^FW wins, also inside one field.
cases['text-explicit-n-then-fw-r']='^FO200,40^A0N,40,40^FWR^FDABC^FS'
cases['text-font-then-fw-r-then-fo']='^A0N,40,40^FWR^FO200,40^FDABC^FS'
cases['bc-explicit-n-then-fw-r']='^BY2^FO200,40^BCN,60,N,N^FWR^FD12345^FS'
cases['bc-omitted-then-fw-r']='^BY2^FO200,40^BC,60,N,N^FWR^FD12345^FS'
cases['bx-explicit-n-then-fw-r']='^FO200,40^BXN,4,200^FWR^FDABC123^FS'
cases['fw-r-text-next-field']='^FWR^FO200,40^A0N,40,40^FDABC^FS^FO400,40^FDDEF^FS'
# Interpretation lines above and below the bars at ^BY1..^BY5, one symbol per
# module width stacked on the label.
for name,command,data in [('be','^BEN,12,Y,Y','123456789012'),('b8','^B8N,12,Y,Y','1234567'),
                          ('bu','^BUN,12,Y,Y','03600029145'),('b9','^B9N,12,Y,Y','0425261'),
                          ('b2','^B2N,12,Y,Y,Y','1234567'),('ba','^BAN,12,Y,Y,Y','ABC123'),
                          ('bi','^BIN,12,Y,Y','123')]:
    body='';y=0
    for module in range(1,6):
        y+=10*module+10
        body+=f'^BY{module}^FO40,{y}{command}^FD{data}^FS'
        y+=31
    cases[f'{name}-above-by1-5']=body
for name,command,data in [('b2','^B2N,20,Y,N,Y','1234567'),('ba','^BAN,20,Y,N,Y','ABC123'),
                          ('bi','^BIN,20,Y,N','123')]:
    body='';y=10
    for module in range(1,6):
        body+=f'^BY{module}^FO40,{y}{command}^FD{data}^FS'
        y+=32+9*module
    cases[f'{name}-below-by1-5']=body
# Aztec size 0 and out-of-range sizes against the omitted default.
cases['bo-size-omitted']='^FO40,40^BON,4,N,,N^FDABC123^FS'
cases['bo-size-invalid']='^FO40,40^BON,4,N,150,N^FDABC123^FS'
# Codabar start/stop characters come from the ^BK k and l parameters.
cases['bk-start-stop-default']='^BY2^FO40,40^BKN,N,60,N,N^FD123^FS'
cases['bk-start-stop-params']='^BY2^FO40,40^BKN,N,60,N,N,B,C^FD123^FS'
cases['bk-start-stop-in-data']='^BY2^FO40,40^BKN,N,60,N,N,B,C^FDD123A^FS'
cases['bk-start-stop-inner']='^BY2^FO40,40^BKN,N,60,N,N^FD1B2^FS'
cases['bk-start-stop-lower']='^BY2^FO40,40^BKN,N,60,N,N,b,c^FD123^FS'
cases['bk-start-stop-invalid']='^BY2^FO40,40^BKN,N,60,N,N,X,Y^FD123^FS'
cases['bk-no-data']='^BY2^FO40,40^BKN,N,60,N,N^FDXYZ^FS^FO40,200^BKN,N,60,N,N^FDAB^FS'
cases['bk-symbols']='^BY2^FO40,40^BKN,N,60,N,N,D,B^FD1-2$3:4/5.6+7^FS'
# EAN-13 digits below the bars at ^BY5: bar height against the data length.
for module in range(1,6):
    cases[f'be-below-by{module}-h110']=f'^BY{module}^FO100,50^BEN,110,Y,N^FD5901234123457^FS'
for rotation in 'NRIB':
    cases[f'be-rotation-{rotation.lower()}']=f'^BY3^FO200,60^BE{rotation},110,Y,N^FD590123412345^FS'
cases['be-below-by5-h20']='^BY5^FO100,50^BEN,20,Y,N^FD5901234123457^FS'
cases['be-below-by5-h110-12']='^BY5^FO100,50^BEN,110,Y,N^FD590123412345^FS'
# ^BY6..10 (and EAN-13 below from ^BY5) need wider labels, in inches.
sizes={}
for name,command,data,above,first in [('be','^BEN,20,Y,N','123456789012',False,5),('be','^BEN,12,Y,Y','123456789012',True,6),
                                      ('b8','^B8N,12,Y,Y','1234567',True,6),('b2','^B2N,20,Y,N,Y','1234567',False,6),
                                      ('b2','^B2N,12,Y,Y,Y','1234567',True,6)]:
    body='';y=0 if above else 10
    for module in range(first,11):
        if above:y+=10*module+10
        body+=f'^BY{module}^FO150,{y}{command}^FD{data}^FS'
        y+=31 if above else 32+9*module
    key=f'{name}-{"above" if above else "below"}-by{first}-10'
    cases[key]=body;sizes[key]=(6,4) if above else (6,3)

def labelary(zpl,inches,dots):
    # Labelary sizes a label as inches * 203 dots at 8 dpmm.
    url=f'https://api.labelary.com/v1/printers/8dpmm/labels/{inches[0]}x{inches[1]}/0/'
    request=urllib.request.Request(url,data=zpl.encode(),headers={'Accept':'image/png','X-Quality':'Bitonal','X-Linter':'On','Content-Type':'application/x-www-form-urlencoded'})
    for attempt in range(4):
        try:
            with urllib.request.urlopen(request,timeout=45) as response:
                data=response.read();warnings=response.headers.get('X-Warnings','')
            break
        except urllib.error.HTTPError as error:
            if error.code!=429 or attempt==3:raise
            time.sleep(3*(attempt+1))
    assert data[:8]==b'\x89PNG\r\n\x1a\n'
    assert struct.unpack('>IIBB',data[16:26])[:3]==(*dots,1)
    time.sleep(1.2)
    return data,warnings

provenance=OUT/'provenance.json'
records=json.loads(provenance.read_text())['responses'] if provenance.exists() else []
for name,body in cases.items():
    inches=sizes.get(name,(3,2));dots=tuple(value*203 for value in inches)
    zpl=f'^XA^PW{dots[0]}^LL{dots[1]}^LH0,0{body}^XZ'
    zp=OUT/(name+'.zpl');png=OUT/(name+'-labelary-bitonal.png')
    if png.exists() and zp.exists() and zp.read_text().strip()==zpl:continue
    data,warnings=labelary(zpl,inches,dots)
    zp.write_text(zpl+'\n',encoding='utf-8',newline='\n')
    png.write_bytes(data)
    records=[entry for entry in records if entry['name']!=name]
    records.append({'name':name,'warnings':warnings})
    print(name,warnings,flush=True)
for entry in records:
    if entry['name'] in sizes:entry['size']='{}x{}'.format(*sizes[entry['name']])
records.sort(key=lambda entry:entry['name'])
provenance.write_text(json.dumps({'source':'Labelary API','date':time.strftime('%Y-%m-%d'),'dpmm':8,'size':'3x2','quality':'Bitonal','responses':records},indent=2)+'\n',encoding='utf-8',newline='\n')

if '--refresh-ean13' in sys.argv[1:]:
    golden=ROOT/'tests/golden'
    for name in ['ean-module-3','ean-module-4','ean-module-5','ocr-b',
                 'ean-rotation-n','ean-rotation-r','ean-rotation-i','ean-rotation-b']:
        png=golden/(name+'-labelary-bitonal.png')
        dots=struct.unpack('>II',png.read_bytes()[16:24])
        inches=tuple(f'{math.ceil(value/203*1e6)/1e6:.6f}' for value in dots)
        data,warnings=labelary((golden/(name+'.zpl')).read_text(encoding='utf-8'),inches,dots)
        png.write_bytes(data)
        print(name,warnings,flush=True)
