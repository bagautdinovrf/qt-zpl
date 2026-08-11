#!/usr/bin/env python3
"""Fetch reviewed-input Labelary bitonal PNGs for the versioned go-zpl corpus.

This tool is intentionally never invoked by CMake or the test suite. It is a
manual maintainer operation used only when preparing or reviewing goldens.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import struct
import time
import urllib.error
import urllib.request


PROJECT_ROOT = Path(__file__).resolve().parent.parent
CORPUS_DIR = PROJECT_ROOT / "tests" / "corpus" / "go-zpl-demo"
GOLDEN_DIR = PROJECT_ROOT / "tests" / "golden" / "go-zpl-demo"


def labelary_input(zpl: bytes, ignore_label_home: bool) -> bytes:
    """Apply web-demo render options that Labelary has no request parameter for."""
    if not ignore_label_home:
        return zpl
    # go-zpl's demo passes ignoreLabelHome=true. Keep the versioned fixture
    # byte-exact, but neutralize ^LH only in the reference-render request so
    # the resulting PNG represents the same render options as the demo.
    return re.sub(rb"\^LH[^\^~\r\n]*", b"^LH0,0", zpl)


def png_properties(payload: bytes) -> tuple[int, int, int, int]:
    if payload[:8] != b"\x89PNG\r\n\x1a\n" or payload[12:16] != b"IHDR":
        raise RuntimeError("Labelary response is not a PNG")
    width, height, bit_depth, color_type = struct.unpack(">IIBB", payload[16:26])
    return width, height, bit_depth, color_type


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--replace", action="store_true", help="replace existing reviewed goldens")
    parser.add_argument("--delay", type=float, default=1.05, help="delay between API calls")
    args = parser.parse_args()
    manifest = json.loads((CORPUS_DIR / "manifest.json").read_text(encoding="utf-8"))
    GOLDEN_DIR.mkdir(parents=True, exist_ok=True)
    requests_made = 0
    for entry in manifest["examples"]:
        zpl = (CORPUS_DIR / entry["fixture"]).read_bytes()
        reference_zpl = labelary_input(zpl, entry["ignoreLabelHome"])
        dpmm = round(entry["dpi"] / 25.4)
        size = f'{entry["widthInches"]:g}x{entry["heightInches"]:g}'
        stem = Path(entry["fixture"]).stem
        for page in range(entry["pages"]):
            output = GOLDEN_DIR / f"{stem}-page-{page + 1}-labelary-bitonal.png"
            if output.exists() and not args.replace:
                print(f"Keeping {output.name}")
                continue
            if requests_made:
                time.sleep(args.delay)
            url = f"https://api.labelary.com/v1/printers/{dpmm}dpmm/labels/{size}/{page}/"
            request = urllib.request.Request(
                url, data=reference_zpl, method="POST",
                headers={"Accept": "image/png", "X-Quality": "Bitonal", "Content-Type": "application/x-www-form-urlencoded"},
            )
            try:
                with urllib.request.urlopen(request, timeout=60) as response:
                    payload = response.read()
            except urllib.error.HTTPError as error:
                details = error.read().decode("utf-8", errors="replace")
                raise RuntimeError(f"Labelary failed for {entry['key']} page {page + 1}: {error.code} {details}") from error
            width, height, bit_depth, color_type = png_properties(payload)
            expected = (entry["widthDots"], entry["heightDots"])
            if (width, height) != expected:
                raise RuntimeError(f"Unexpected size for {entry['key']} page {page + 1}: {(width, height)} != {expected}")
            # Labelary Bitonal returns a 1-bit grayscale or indexed PNG. Reject
            # 8-bit grayscale/RGB responses so the API cannot silently weaken goldens.
            if bit_depth != 1 or color_type not in (0, 3):
                raise RuntimeError(f"Non-bitonal PNG for {entry['key']} page {page + 1}: depth={bit_depth}, type={color_type}")
            output.write_bytes(payload)
            requests_made += 1
            print(f"Fetched {output.name}: {width}x{height}, bitonal {bit_depth}-bit")
    print(f"Fetched {requests_made} golden page(s)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
