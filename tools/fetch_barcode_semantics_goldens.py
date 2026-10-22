"""Manual maintainer tool. Normal builds/tests never access Labelary.

Fetches bitonal Labelary references for barcode parameter defaults, Y/N case
and ^FW inheritance (Designer stage 2, discrepancies D1-D6).
"""
from pathlib import Path
import json
import struct
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

provenance=OUT/'provenance.json'
records=json.loads(provenance.read_text())['responses'] if provenance.exists() else []
for name,body in cases.items():
    zpl=f'^XA^PW609^LL406^LH0,0{body}^XZ'
    zp=OUT/(name+'.zpl');png=OUT/(name+'-labelary-bitonal.png')
    if png.exists() and zp.exists() and zp.read_text().strip()==zpl:continue
    url='https://api.labelary.com/v1/printers/8dpmm/labels/3x2/0/'
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
    assert struct.unpack('>IIBB',data[16:26])[:3]==(609,406,1)
    zp.write_text(zpl+'\n',encoding='utf-8',newline='\n')
    png.write_bytes(data)
    records=[entry for entry in records if entry['name']!=name]
    records.append({'name':name,'warnings':warnings})
    print(name,warnings,flush=True)
    time.sleep(1.2)
records.sort(key=lambda entry:entry['name'])
provenance.write_text(json.dumps({'source':'Labelary API','date':time.strftime('%Y-%m-%d'),'dpmm':8,'size':'3x2','quality':'Bitonal','responses':records},indent=2)+'\n',encoding='utf-8',newline='\n')
