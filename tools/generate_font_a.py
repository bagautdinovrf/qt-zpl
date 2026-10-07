"""Generate the licensed Font A derivative; no Labelary font data is an input.

Run with FontForge's Python interpreter, then the official ttfautohint tool:
  fontforge -lang=py -script tools/generate_font_a.py --ttfautohint PATH
Requires FontForge 20251009 and ttfautohint 1.8.4 for the checked-in result.
"""
import argparse
import re
import struct
import subprocess
import tempfile
from pathlib import Path

import fontforge

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--ttfautohint", required=True)
parser.add_argument("--output")
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
output = Path(args.output) if args.output else root / "resources/fonts/font_a_native.ttf"
source = root / "resources/fonts/font_a.ttf"
metrics_text = (root / "src/printer_font_metrics.hpp").read_text(encoding="utf-8")
metrics = {
    int(match[4]): tuple(map(float, match[:4]))
    for match in re.findall(r"\{([-\d.]+), ([-\d.]+), ([-\d.]+), ([-\d.]+)\}, // (\d+)", metrics_text)
}
assert len(metrics) == 95
metrics[0x25AF] = (35 / 33, 35 / 33, -82, 0)

font = fontforge.open(str(source))
font.selection.all()
font.unlinkReferences()
for glyph in font.glyphs():
    character = 92 if glyph.unicode == 162 else glyph.unicode
    sx, sy, dx, dy = metrics.get(character, (35 / 33, 35 / 33, 0, 0))
    glyph.transform((sx, 0, 0, sy, dx, dy))
    glyph.round()
    glyph.width = 1365
font.fontname = "QtZplFontANative"
font.familyname = "QtZpl Font A Native"
font.fullname = "QtZpl Font A Native"
font.selection.all()
font.autoHint()
font.autoInstr()
with tempfile.TemporaryDirectory(prefix="qtzpl-font-a-") as temporary:
    intermediate = Path(temporary) / "normalized.ttf"
    font.generate(str(intermediate))
    font.close()
    # Disable the screen-font x-height boost: printer dots must stay literal.
    subprocess.run([args.ttfautohint, "-x", "0", str(intermediate), str(output)], check=True)
print(str(output))

# Only timestamps vary between otherwise identical FontForge runs. Normalize
# their tables and recompute SFNT checksums without another Python dependency.
data = bytearray(output.read_bytes())
tables = {}
for index in range(struct.unpack_from(">H", data, 4)[0]):
    entry = 12 + index * 16
    tag, _, offset, length = struct.unpack_from(">4sIII", data, entry)
    tables[tag] = (entry, offset, length)
head = tables[b"head"][1]
struct.pack_into(">I", data, head + 8, 0)
struct.pack_into(">QQ", data, head + 20, 3786912000, 3786912000)  # 2024-01-01
if b"FFTM" in tables:
    offset = tables[b"FFTM"][1]
    struct.pack_into(">QQQ", data, offset + 4, 3786912000, 3786912000, 3786912000)

def checksum(block):
    block = bytes(block) + bytes((-len(block)) % 4)
    return sum(struct.unpack(">" + "I" * (len(block) // 4), block)) & 0xFFFFFFFF

for entry, offset, length in tables.values():
    struct.pack_into(">I", data, entry + 4, checksum(data[offset:offset + length]))
struct.pack_into(">I", data, head + 8, (0xB1B0AFBA - checksum(data)) & 0xFFFFFFFF)
output.write_bytes(data)
