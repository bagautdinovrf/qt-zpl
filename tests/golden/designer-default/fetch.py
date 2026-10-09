"""Manually refresh Labelary references; never run from builds or tests.

Pass --designer-export only when intentionally updating the default fixture.
The ordinary invocation re-fetches the existing committed ZPL byte-for-byte.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import struct
import time
import urllib.error
import urllib.request
import zlib


ROOT = Path(__file__).resolve().parent
HEADERS = {
    "Accept": "image/png",
    "Content-Type": "application/x-www-form-urlencoded",
    "X-Quality": "Bitonal",
    "X-Linter": "On",
}
INCHES = f"{(799 + 0.25) / 203:.9f}"
URL = f"https://api.labelary.com/v1/printers/8dpmm/labels/{INCHES}x{INCHES}/0/"


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def validate_png(data: bytes) -> None:
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "Not a PNG response"
    assert struct.unpack(">IIBB", data[16:26]) == (799, 799, 1, 0), "Expected 799x799 1-bit grayscale"
    position = 8
    while position < len(data):
        length = struct.unpack(">I", data[position:position + 4])[0]
        end = position + 8 + length
        assert end + 4 <= len(data), "Truncated PNG chunk"
        checksum = struct.unpack(">I", data[end:end + 4])[0]
        assert zlib.crc32(data[position + 4:end]) & 0xFFFFFFFF == checksum, "Invalid PNG CRC"
        position = end + 4
    assert position == len(data), "Trailing incomplete PNG chunk"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--designer-export", type=Path)
    parser.add_argument("--designer-commit", default="")
    parser.add_argument("--case", action="append", choices=("northline", "northline-text", "northline-graphics"))
    args = parser.parse_args()
    provenance_path = ROOT / "provenance.json"
    provenance = json.loads(provenance_path.read_text(encoding="utf-8")) if provenance_path.exists() else {}
    if args.designer_export:
        exported = args.designer_export.read_bytes()
        source, count = re.subn(rb"(?<=\^XA)\^FXQTZPL_EDITOR_V2:[A-Za-z0-9+/=]+\^FS", b"", exported, count=1)
        assert count == 1, "Expected one leading Designer V2 metadata comment"
        assert b"QTZPL_EDITOR" not in source
        assert source.startswith(b"^XA\n^CI28\n^PW799\n^LL799\n^LH0,0\n")
        (ROOT / "northline.zpl").write_bytes(source)
        provenance["fixtureSource"] = {
            "repository": "qt-zpl-designer",
            "commit": args.designer_commit,
            "constructor": "src/core/design_document.cpp:sampleDocument()",
            "export": "toZpl(sampleDocument())",
            "exportedFile": args.designer_export.name,
            "exportedSha256": sha256(exported),
            "modification": "Removed only the leading ^FXQTZPL_EDITOR_V2 metadata comment through its ^FS; all other bytes unchanged.",
        }

    full = (ROOT / "northline.zpl").read_bytes()
    lines = full.splitlines(keepends=True)
    header = b"".join(lines[:5])
    assert header == b"^XA\n^CI28\n^PW799\n^LL799\n^LH0,0\n"
    fields = [line for line in lines[5:-1] if b"^A0" in line]
    assert len(fields) == 26
    assert all(line.startswith(b"^FO") and b"^FD" in line and line.endswith(b"^FS\n") for line in fields)
    assert lines[-1] == b"^XZ\n"
    text = header + b"".join(fields) + lines[-1]
    (ROOT / "northline-text.zpl").write_bytes(text)
    graphics = [line for line in lines[5:-1] if b"^GB" in line or b"^GFA" in line]
    assert all(line.startswith(b"^FO") and line.endswith(b"^FS\n") for line in graphics)
    assert sum(b"^GFA" in line for line in graphics) == 2
    (ROOT / "northline-graphics.zpl").write_bytes(header + b"".join(graphics) + lines[-1])

    responses = provenance.get("responses", [])
    for name in args.case or ("northline", "northline-text", "northline-graphics"):
        source = (ROOT / f"{name}.zpl").read_bytes()
        request = urllib.request.Request(URL, data=source, headers=HEADERS)
        for attempt in range(4):
            try:
                with urllib.request.urlopen(request, timeout=45) as response:
                    data = response.read()
                    warnings = response.headers.get("X-Warnings", "")
                    response_date = response.headers.get("Date", "")
                break
            except urllib.error.HTTPError as error:
                if error.code != 429 or attempt == 3:
                    raise
                delay = max(3 * (attempt + 1), int(error.headers.get("Retry-After", "0")))
                if delay > 60:
                    raise
                time.sleep(delay)
        validate_png(data)
        (ROOT / f"{name}-labelary-bitonal.png").write_bytes(data)
        responses = [entry for entry in responses if entry["name"] != name]
        responses.append({
            "name": name,
            "retrievedUtc": datetime.now(timezone.utc).isoformat(),
            "zplSha256": sha256(source),
            "pngSha256": sha256(data),
            "pngBytes": len(data),
            "warnings": warnings,
            "labelaryResponseDate": response_date,
        })
        print(f"{name}: verified 799x799 bitonal PNG ({len(data)} bytes)", flush=True)
        time.sleep(1.2)

    provenance.update({
        "source": "Labelary API",
        "url": URL,
        "method": "POST",
        "requestHeaders": HEADERS,
        "renderOptions": {"width": 799, "height": 799, "dpi": 203},
        "imageValidation": "PNG signature, 799x799 IHDR, bit depth 1, grayscale color type 0, every chunk CRC. Stored unmodified response bytes.",
        "textDerivative": "The 26 complete ^FO...^A0...^FD...^FS lines in their original order, plus unchanged ^XA/^CI28/^PW/^LL/^LH/^XZ. Other fields, including the black backing for reverse B1, are omitted; its ^FR remains unchanged.",
        "graphicsDerivative": "Complete ^FO...^GB...^FS and ^FO...^GFA...^FS lines in their original order, plus unchanged ^XA/^CI28/^PW/^LL/^LH/^XZ. Text and all three barcodes are omitted.",
        "responses": responses,
    })
    provenance_path.write_text(json.dumps(provenance, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
