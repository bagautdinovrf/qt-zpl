# MaxiCode modes 3–6 and structured append

Original Labelary bitonal images downloaded on 2026-10-07 with their matching
ZPL. API: `https://api.labelary.com/v1/printers/8dpmm/labels/2x2/0/`;
headers: `Accept: image/png`, `X-Quality: Bitonal`, `X-Linter: On`.
The original responses are 406×406, 1-bit PNGs. All requests were warning-free.

Mode 3 uses the Zebra documented service/country/postal prefix and an ISO
15434 carrier envelope. Modes 4–6 use identical bytes to expose their differing
mode bits and (mode 5) correction capacity. The append case is part 2 of 3.

`ft-mode4` fixes the baseline anchor at `^FT20,250`: ink bounds are
`(20,56 200x192)`, compared with `(20,21 200x192)` for `^FO20,20`.
The raster has a 195-dot layout height with natural blank pixels above and
below its visible modules. The fixed geometry is covered independently of
encoding, including integer hexagon vertices and finder scan conversion.
