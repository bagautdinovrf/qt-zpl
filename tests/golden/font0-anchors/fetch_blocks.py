"""Manual refresh of the independent Font 0 FT+FB rotation oracle."""
from datetime import datetime, timezone
from pathlib import Path
import hashlib,json,struct,urllib.request,zlib

ROOT=Path(__file__).resolve().parent
WIDTH,HEIGHT,CELL=1536,1152,384
URL=f'https://api.labelary.com/v1/printers/8dpmm/labels/{(WIDTH+.25)/203:.9f}x{(HEIGHT+.25)/203:.9f}/0/'
HEADERS={'Accept':'image/png','Content-Type':'application/x-www-form-urlencoded','X-Quality':'Bitonal'}

def main():
    fields=['^XA','^CI28',f'^PW{WIDTH}',f'^LL{HEIGHT}','^LH0,0'];cases=[]
    for row,(height,width,max_lines,spacing,text) in enumerate(((17,17,1,0,'HIETgЖя'),(17,17,3,2,'HIET\\&gЖя'),(64,23,3,3,'HIET\\&gЖя'))):
        for col,orientation in enumerate('NRIB'):
            x,y=col*CELL+CELL//2,row*CELL+CELL//2
            fields.append(f'^FT{x},{y}^A0{orientation},{height},{width}^FB100,{max_lines},{spacing},L,0^FD{text}^FS')
            cases.append(dict(row=row,orientation=orientation,height=height,width=width,maxLines=max_lines,lineSpacing=spacing,
                              text=text,cell=[col*CELL,row*CELL,CELL,CELL],origin=[x,y]))
    source=('\n'.join(fields+['^XZ',''])).encode('utf-8');(ROOT/'block-anchors.zpl').write_bytes(source)
    with urllib.request.urlopen(urllib.request.Request(URL,data=source,headers=HEADERS),timeout=45) as response:
        data=response.read();response_date=response.headers.get('Date','')
    assert data[:8]==b'\x89PNG\r\n\x1a\n' and struct.unpack('>IIBB',data[16:26])==(WIDTH,HEIGHT,1,0)
    offset=8
    while offset<len(data):
        length=struct.unpack('>I',data[offset:offset+4])[0];end=offset+8+length
        assert end+4<=len(data) and zlib.crc32(data[offset+4:end])&0xffffffff==struct.unpack('>I',data[end:end+4])[0]
        offset=end+4
    assert offset==len(data)
    (ROOT/'block-anchors-labelary-bitonal.png').write_bytes(data)
    (ROOT/'block-provenance.json').write_text(json.dumps(dict(source='Labelary API',url=URL,method='POST',requestHeaders=HEADERS,
        retrievedUtc=datetime.now(timezone.utc).isoformat(),responseDate=response_date,zplSha256=hashlib.sha256(source).hexdigest(),
        pngSha256=hashlib.sha256(data).hexdigest(),validation='PNG signature, exact dimensions, grayscale bit depth 1, every chunk CRC; unmodified response.',cases=cases),ensure_ascii=False,indent=2)+'\n','utf-8')
    print('block-anchors',len(data),'verified original bitonal PNG',flush=True)

if __name__=='__main__':main()
