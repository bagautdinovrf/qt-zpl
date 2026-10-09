# Font 0 baseline rotation anchors

Original, unmodified Labelary responses requested with `X-Quality: Bitonal`.
`fetch.py` is an explicit manual refresh tool and is never run by normal tests.
`provenance.json` records request settings, SHA-256 and PNG validation.

Two 576×2016 atlases exercise heights/widths 17/17 and 64/23. Each contains
H, I, E, T, g, Ж and я as independent glyphs, FO then FT, with N/R/I/B columns.
Every 144×144 cell places its field origin at (72,72), away from label edges.

The FT reference establishes rotation around the pixel-edge origin. Rotating
the normal 144×144 cell matches every golden pixel at height 17. At height 64,
all rotated ink bounds agree; the reference scan converter changes a few
boundary pixels (R: 5, I: 5, B: 2 in total across the seven glyphs).

FO minus FT placement perpendicular to the baseline is independent of glyph
shape and advance: N/B use `floor(0.75*height)` (12 or 48); R/I use height minus
that value (5 or 16). This isolates anchor semantics from Font 0 contour and
horizontal-metric discrepancies.

The pre-fix QtZpl renderer instead shifted its rotated FT bitmap by R=(1,0),
I=(1,1), B=(0,1) relative to rotation of its own normal glyph. The focused test
checks the independent oracle relation and the same relation in QtZpl.

`block-anchors.zpl` independently checks `^FT` combined with `^FB`: one
17-dot line, two 17-dot lines with spacing 2, and two 64×23-dot lines with
spacing 3. Both multiline cases reserve three lines, exercising the logical
baseline after an unpainted line. Four columns again use N/R/I/B, with the
origin at the center of each 384×384 cell. The oracle establishes the same
edge-based relation. `fetch_blocks.py` refreshes this separate unmodified
bitonal response and `block-provenance.json` records its source and hashes.
