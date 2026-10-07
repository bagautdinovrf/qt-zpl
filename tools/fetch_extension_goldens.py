"""Manual maintainer tool. Normal builds/tests never access Labelary."""
from pathlib import Path
import json
import struct
import time
import urllib.request
import urllib.error

ROOT=Path(__file__).resolve().parents[1]
OUT=ROOT/'tests/golden/extensions'
OUT.mkdir(exist_ok=True)
cases={}
for code,data in [('B8','9638507'),('B9','0425261'),('BU','03600029145'),('BA','ABC-12345'),('B2','12345678'),('BI','12345678')]:
    for orientation in 'NRIB':
        cases[f'{code.lower()}-{orientation.lower()}']=f'^BY3,2.5^FO160,170^{code}{orientation},70,N,N^FD{data}^FS'
        cases[f'{code.lower()}-ft-{orientation.lower()}']=f'^BY3,2.5^FT160,170^{code}{orientation},70,N,N^FD{data}^FS'
    cases[f'{code.lower()}-caption']=f'^BY3,2.5^FO80,70^{code}N,70,Y,N^FD{data}^FS'
    cases[f'{code.lower()}-above']=f'^BY3,2.5^FO80,100^{code}N,70,Y,Y^FD{data}^FS'
body='^FO20,30^GB80,30,30^FS^FO140,60^GB25,55,25^FS^FO230,80^GFA,4,4,1,A050A050^FS'
for name,control in [('normal',''),('top','^LT45'),('mirror','^PMY'),('reverse','^LRY'),('reverse-fr','^LRY^FR')]:
    cases['transform-'+name]=control+body
cases['reverse-on-black']='^FO0,0^GB609,406,406^FS^LRY'+body
cases['reverse-overlap']='^LRY^FO20,30^GB80,30,30^FS^FO20,30^GB80,30,30^FS^LRN^FO140,60^GB25,55,25^FS'
for direction in 'HVR':
    for spacing in [0,5]:
        cases[f'fp-{direction.lower()}-{spacing}']=f'^FO100,90^AAN,27,15^FP{direction},{spacing}^FDABC123^FS'
for code,parameters in [('GB','50,30,2'),('GC','30,2'),('GE','50,30,2'),('GD','50,30,2,B,R'),('GF','A,4,4,1,A050A050')]:
    cases[f'ft-{code.lower()}']=f'^FT150,150^{code}{parameters}^FS'
cases['font-a-atlas']=''.join(f'^FO{20+(i%16)*24},{20+(i//16)*20}^AAN,9,5^FH_^FD_{i+32:02X}^FS' for i in range(95))
for height,width in [(10,5),(20,10),(30,0),(50,50)]:
    cases[f'font-a-h{height}-w{width}']=f'^FO100,90^AAN,{height},{width}^FDABC123^FS'
for height,width in [(25,25),(40,40),(60,30)]:
    for name,character in [('hyphen','-'),('en-dash','–'),('em-dash','—'),('minus','−')]:
        cases[f'font0-{name}-h{height}-w{width}']=f'^CI28^FT100,100^A0N,{height},{width}^FD{character}^FS'
for origin in ['FO','FT']:
    for rotation in 'NRIB':
        for justification in [0,1,2]:
            cases[f'{origin.lower()}-text-{rotation.lower()}-{justification}']=f'^{origin}300,170,{justification}^AA{rotation},27,15^FDABC123^FS'
    cases[f'{origin.lower()}-right-box']=f'^{origin}300,170,1^GB50,30,2^FS'
    cases[f'{origin.lower()}-fw-right']=f'^FWN,1^{origin}300,170^AAN,27,15^FDABC123^FS'
cases['ft-omitted']='^FT100,90^AAN,27,15^FDABC^FS^FT^FD123^FS^FT,150^FDDEF^FS'
for code,parameters,data in [('BX','3,200','ABC'),('BQ','2,3','QA,ABC'),('B7','3,2,3,5,N','ABC')]:
    for rotation in 'NRIB':
        cases[f'ft-{code.lower()}-{rotation.lower()}']=f'^BY2,3,1^FT300,170^{code}{rotation},{parameters}^FD{data}^FS'

provenance=OUT/'provenance.json'
records=json.loads(provenance.read_text())['responses'] if provenance.exists() else []
for name,body in cases.items():
    zpl=f'^XA^PW609^LL406^LH0,0{body}^XZ'
    zp=OUT/(name+'.zpl');png=OUT/(name+'-labelary-bitonal.png')
    if png.exists() and zp.exists() and zp.read_text().strip()==zpl:continue
    zp.write_text(zpl+'\n',encoding='utf-8')
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
    png.write_bytes(data)
    records=[entry for entry in records if entry['name']!=name]
    records.append({'name':name,'warnings':warnings})
    print(name,warnings,flush=True)
    time.sleep(1.2)
(OUT/'provenance.json').write_text(json.dumps({'source':'Labelary API','date':'2026-10-07','dpmm':8,'size':'3x2','quality':'Bitonal','responses':records},indent=2))
