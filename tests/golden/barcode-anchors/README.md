# EAN-13 and Code 39 origins

Original Labelary bitonal responses, downloaded 2026-10-07 from the 8 dpmm
2×2 inch API. Headers: `Accept: image/png`, `X-Quality: Bitonal`, `X-Linter: On`.
Matching ZPL inputs are retained; every final response was warning-free.

For each symbology, the fixtures use the same ^FO or ^FT position and four
orientations. They test the barcode bar-base anchor separately from text:
interpretation is disabled. Code 39 also exercises clipping at the right,
bottom, left or top label edge. ^BY explicitly fixes width and ratio at 2.
