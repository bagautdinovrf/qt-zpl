"""Manual Labelary fixture refresh; never called by builds or tests."""
from datetime import datetime, timezone
from pathlib import Path
import hashlib, json, struct, time, urllib.error, urllib.request, zlib

ROOT=Path(__file__).resolve().parent
HEADERS={'Accept':'image/png','Content-Type':'application/x-www-form-urlencoded','X-Quality':'Bitonal'}
WIDTH,HEIGHT,CELL=576,2016,144
URL=f'https://api.labelary.com/v1/printers/8dpmm/labels/{(WIDTH+.25)/203:.9f}x{(HEIGHT+.25)/203:.9f}/0/'
CHARACTERS='HIETgЖя'

def main():
    records=[];cases=[]
    for height,width in ((17,17),(64,23)):
        name=f'anchors-h{height}-w{width}'
        fields=['^XA','^CI28',f'^PW{WIDTH}',f'^LL{HEIGHT}','^LH0,0']
        for anchor_index,anchor in enumerate(('FO','FT')):
            for index,char in enumerate(CHARACTERS):
                row=anchor_index*len(CHARACTERS)+index
                for column,orientation in enumerate('NRIB'):
                    x,y=column*CELL+CELL//2,row*CELL+CELL//2
                    fields.append(f'^{anchor}{x},{y}^A0{orientation},{height},{width}^FD{char}^FS')
                    cases.append(dict(name=name,anchor=anchor,orientation=orientation,height=height,width=width,character=char,
                                      cell=[column*CELL,row*CELL,CELL,CELL],origin=[x,y]))
        source=('\n'.join(fields+['^XZ',''])).encode('utf-8')
        (ROOT/f'{name}.zpl').write_bytes(source)
        request=urllib.request.Request(URL,data=source,headers=HEADERS)
        for attempt in range(4):
            try:
                with urllib.request.urlopen(request,timeout=45) as response:
                    data=response.read();response_date=response.headers.get('Date','');warnings=response.headers.get('X-Warnings','')
                break
            except urllib.error.HTTPError as e:
                if e.code!=429 or attempt==3:raise
                delay=max(3*(attempt+1),int(e.headers.get('Retry-After','0')))
                if delay>60:raise
                time.sleep(delay)
        assert data[:8]==b'\x89PNG\r\n\x1a\n'
        assert struct.unpack('>IIBB',data[16:26])==(WIDTH,HEIGHT,1,0)
        offset=8
        while offset<len(data):
            length=struct.unpack('>I',data[offset:offset+4])[0];end=offset+8+length
            assert end+4<=len(data)
            assert zlib.crc32(data[offset+4:end])&0xffffffff==struct.unpack('>I',data[end:end+4])[0]
            offset=end+4
        assert offset==len(data)
        (ROOT/f'{name}-labelary-bitonal.png').write_bytes(data)
        records.append(dict(name=name,zplSha256=hashlib.sha256(source).hexdigest(),pngSha256=hashlib.sha256(data).hexdigest(),
                            retrievedUtc=datetime.now(timezone.utc).isoformat(),responseDate=response_date,warnings=warnings))
        print(name,len(data),'verified original bitonal PNG',flush=True)
        time.sleep(1.2)
    (ROOT/'cases.json').write_text(json.dumps(cases,ensure_ascii=False,indent=2)+'\n','utf-8')
    (ROOT/'provenance.json').write_text(json.dumps(dict(source='Labelary API',url=URL,method='POST',requestHeaders=HEADERS,
        size=[WIDTH,HEIGHT],validation='PNG signature, dimensions, bit depth 1, grayscale color type 0, all chunk CRCs. Original response bytes.',responses=records),indent=2)+'\n','utf-8')

if __name__=='__main__':main()
