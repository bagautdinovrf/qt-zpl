# Font 0 advance and layout measurements

These are original Labelary `X-Quality: Bitonal` responses. They are numerical
layout references, not a claim that QtZpl letter contours match Labelary.
`manifest.json` and `layout-holdout.json` record the exact API requests, response
dates, warnings, dimensions and SHA-256 hashes. No response was thresholded,
resized, retouched or recompressed. Label dimensions use `(dots + 0.25) / 203`
inches at 8 dpmm to retain the requested integer PNG dimensions.

`measurements.json` records exact 2048-unit horizontal advance numerators for
317 characters. The numerical source is a diagnostic Labelary PDF generated
from `numerical-metadata.zpl`: input character order is paired with PDF `Tj`
CID operands; `CIDToGIDMap` then identifies the numeric `hmtx` advance. Only
these numerical metrics are retained here. The PDF font program, outlines and
hint instructions are neither redistributed nor used as implementation data.
The original diagnostic PDF hash and request provenance are recorded in the
measurement JSON.

Every numerator has two independent original PNG ruler observations. The main
series repeats each character 8 times at width 300 and 24 times at width 100,
followed by a pipe. A standalone pipe on the same page supplies its bearing.
The final pipe shape is checked exactly before measuring its displacement.
Both series span 2400 nominal font-width units, giving an uncertainty of at
most 1/2400 in normalized advance. The per-mille sign uses shorter repetitions
to stay within the label. There are 634 accepted observations; all agree with
the exact numerators within 0.5 printer dot.

Two additional observations are deliberately excluded and preserved: Labelary
silently omits U+2030 and U+2116 at width 300. Their advance would incorrectly
appear to be zero. Both signs pass independent width 100 and width 200 rulers;
the excluded observations have status `oracle-omits-glyph-at-width300`.

`embedded-font-coverage.json` maps these characters to the current bundled
`font0.ttf`. It covers 304 distinct glyph IDs out of 340; 13 measured Unicode
characters are absent from that font. Every other glyph keeps its original
advance. The generator preserves zero advances, rejects invalid integer data,
and leaves conflicting or unmeasured Unicode aliases on the fallback path.
There are no alias conflicts in this font version. Font SHA-256 is checked in
the C++ test, so replacing the bundled face requires regenerating its mapping.

The independent `layout-holdout` contains 30 cases: FT N/R/I/B with horizontal,
vertical and reverse directions, spacing 3, left/right justification, plus FO
N equivalents. Each source text is followed by two partial FT probes. One
carries X into a marker in an otherwise empty top strip; the other carries Y
into an empty left strip. This isolates cursor continuation from glyph shape.
All 60 markers are solid 3 by 11 dots and are found directly in the original
PNG, rather than predicted using the implementation's advance table.

`layout-parameter-holdout` adds 24 cases and 48 isolated markers with independent
height/width/spacing/text profiles: 14/31/0/A, 37/13/1/Test12, 19/47/5/Мир and
26/17/0/Milk. These distinguish a nominal cell from a half-height or arbitrary
width correction. The cell is the nearest integer dot width of M, whose exact
advance is 1548/2048. For total advance `T = sum(advances) + count * spacing`
and nominal cell C, the FT left continuation offsets are T for H/V and T-C for
R. Right placement and continuation offsets are -T for H, -height for V and
T-2C for R; orientation is applied before taking integer world coordinates.
The reverse pen begins at C and subtracts each glyph's advance and spacing
before drawing that glyph. Numeric PDF text origins were checked independently
against these raster marker observations; reference contours were not reused.

`layout-fo-rotations` adds 18 cases and 36 markers for FO R/I/B; the final
`layout-fo-parameter-holdout` adds 42 cases and 84 markers. The latter repeats
reverse text in all orientations and vertical R/I text for three independent
parameter profiles, then checks empty FT fields for all directions and
justifications. There are 228 cursor markers in total. Empty fields preserve
the specified FT anchor with no cell-width or justification displacement.

The final dense page includes valid continuations outside nominal cell edges
and near other text. Its `markerRects` are extracted as original solid 3x11
connected components with a clear perimeter, not predicted from a layout
formula. The C++ test compares their coordinates to the actual renderer's box
paint bounds and verifies solid pixels in both images. This avoids mistaking
neighboring text for the marker.

The independently observed FO rules use effective height h, count n,
`a = floor(0.75*h)`, `W = floor(T)` and the M-based cell C. Let the orientation
unit vector be N=(1,0), R=(0,1), I=(-1,0), B=(0,-1). Coordinates are floored
after orientation and translation into the label's world coordinates.

| Direction | FO baseline offsets, ordered N / R / I / B |
|---|---|
| H | (0,a) / (h-a,0) / (W,h-a) / (a,W) |
| V | (0,a) / ((n-1)h,0) / (C,nh-a) / (a,C) |
| R glyph placement | (0,a) / (h-a,0) / (C,h-a) / (a,C) |
| R cursor perpendicular component P | (0,a) / (h-a,0) / (0,h-a) / (a,0) |

For H, the left cursor is baseline-offset + unit-vector*T. Right FO alignment
shifts physical X by `floor(-T)` for N, -W for I, or -h for R/B, and the cursor
is that shifted anchor plus the baseline offset. For V, the right anchor is shifted along the
orientation vector by -h; its cursor adds the V baseline offset. The V left
cursor adds T along the orientation vector to the same baseline offset.

For reverse text, FO N/R left cursor displacement is P + vector*(T-C), and
right displacement is P + vector*(T-2C). For FO I/B these are P + vector*T
and P + vector*(T-C). Reverse glyphs retain the pre-decrement pen rule; right
placement uses the same right along-axis displacement before adding the full
R glyph baseline offset. These separate glyph and cursor rules are required:
using the same bounding rectangle for both fails the original FO I/B markers.

The metrics test also embeds the seven existing `font0-h*-w*` fixture pairs
from the parent directory. For each of H, g, Milk and the Cyrillic sample,
the original FO and FT masks are identical after translation, X is unchanged,
and the baseline offset is `floor(0.75 * height)`. This is verified at heights
14, 16, 25, 26, 27, 40 and 70, including asymmetric widths. The PDF font
descriptor's cap-height value is not the ZPL FO-to-FT baseline contract.

Manual maintenance, from the repository root:

```text
py -3 tests/golden/font0-metrics/fetch.py
py -3 tests/golden/font0-metrics/fetch_layout.py
py -3 tests/golden/font0-metrics/fetch_layout_parameters.py
py -3 tests/golden/font0-metrics/fetch_layout_fo.py
py -3 tests/golden/font0-metrics/fetch_layout_fo_parameters.py
py -3 tools/generate_font0_advances.py --check
```

The fetch scripts check cached originals offline; `--fetch` permits
acquisition only when a PNG is missing. They require Pillow. Header generation
requires fontTools; neither package nor network access is required by the
library or normal C++ tests. To regenerate the table and coverage mapping after
a deliberate font or measurement update, omit `--check` from the generator.

`qtzpl_font0_metrics_tests` exercises the full renderer against original ruler
endpoints (one-dot tolerance), original cursor markers (one-dot tolerance),
and exact FO/FT paired translations. It also verifies source/PNG hashes,
embedded-font glyph IDs and fallback entries. Remaining contour differences
must be assessed independently using the glyph atlases and NORTHLINE label.
