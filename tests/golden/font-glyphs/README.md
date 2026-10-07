# Font 0 glyph census

These original bitonal PNGs were obtained from Labelary on 2026-10-07 at
8 dpmm, 12×12 inches (2436×2436 dots), using `X-Quality: Bitonal` and
`X-Linter: On`. Every PNG has its exact UTF-8 ZPL input beside it. The fixtures
use `^CI28`; ZPL delimiters and underscore are escaped with `^FH`.

`manifest.json` records every glyph cell and the eight height/width profiles:
12/12, 16/16, 25/25, 30/30, 40/40, 26/33, 60/30, and 70/100. The base pages
cover the embedded font's repertoire plus complete ASCII and Russian alphabets,
including Ё/ё. The special-character pages add common currencies, mathematical
operators, arrows, fractions, quotation marks, and other punctuation. Together
the 16 pages exercise 329 distinct code points in 2920 glyph placements.

Labelary explicitly warns that Font 0 cannot display U+037E, U+2044, U+20BD,
U+2206, U+220F, U+2211, U+221A, U+221E, U+2248, U+2260, U+2264, U+2265,
U+2713, U+2717, U+FB01, and U+FB02. Their reference cells are empty. They
remain in the tests: QtZpl must emit `font-glyph-missing` with the code point
and source position. Silently dropping or replacing them does not pass.

`qtzpl_font_glyphs_tests` compares every cell and the entire page. It writes
`font-glyph-results.json` next to its executable, with per-glyph differences,
ink bounds, diagnostics, and all failed glyph/size combinations. Failed page
rasters are also written in the build directory, never over these references.

Regeneration is manual with `py -3 tools/fetch_font_goldens.py`; normal tests
perform no network calls. The manifest preserves Labelary's warning headers.
