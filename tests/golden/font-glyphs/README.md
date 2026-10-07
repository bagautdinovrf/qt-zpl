# Font 0 glyph census

These original bitonal PNGs were obtained from Labelary on 2026-10-07/08 at
8 dpmm, 12×12 or 15×15 inches (2436×2436 or 3045×3045 dots), using `X-Quality: Bitonal` and
`X-Linter: On`. Every PNG has its exact UTF-8 ZPL input beside it. The fixtures
use `^CI28`; ZPL delimiters and underscore are escaped with `^FH`.

`manifest.json` retains every original glyph cell and the eight height/width profiles:
12/12, 16/16, 25/25, 30/30, 40/40, 26/33, 60/30, and 70/100. The base pages
cover the embedded font's repertoire plus complete ASCII and Russian alphabets,
including Ё/ё. The special-character pages add common currencies, mathematical
operators, arrows, fractions, quotation marks, and other punctuation. Together
the original 16 pages exercise 329 distinct code points in 2920 glyph placements.
Their PNG and ZPL bytes have not been changed by the expanded census.

The expanded repertoire is explicit in `tools/fetch_font_goldens.py`; it does
not depend on the font being tested. It contains all printable ASCII, printable
Latin-1 (excluding the soft-hyphen formatting control), all 66 Russian letters,
the legacy extended letters, and a listed set of common punctuation, currency,
mathematical, and other symbols. This is 371 code points, not a claim to cover
all Unicode. The new entries that Labelary renders are U+2015 HORIZONTAL BAR,
U+2017 DOUBLE LOW LINE, U+203C DOUBLE EXCLAMATION MARK, and U+20AA NEW SHEQEL SIGN.

The manifest has 40 single-glyph atlas cases and 9311 glyph placements:

| Role | Pages | Profiles (height/width, anchor, orientation) |
|---|---:|---|
| `train` | 11 | All 371 code points at the eight original profiles, FT/N |
| `validation` | 10 | 9/9, 17/17, 47/47, 96/96 FT/N; 23/31 FO/N; 31/13 FO/R; 64/23 FT/I; 11/17 FO/B |
| `legacy-validation` | 19 | Original 16 pages, two extra-symbol censuses, and the first 47/47 holdout |

The 47-dot and 96-dot holdouts each span two pages. The 70/100 training profile
spans four pages. Training uses 2968 placements with 0.7 em plus 12 dots of
margin, then checks an empty 10-dot halo inside every cell and the native
probe's additional 10-dot top/left crop. This prevents negative bearings from
being clipped and neighbouring glyphs from contaminating measurements.

The original compact training layout was found to clip U+2215 at 70/100 and
include neighbouring ink in some expanded probe crops, including U+00B4 at
12/12 and U+00A3 at 40/40. Its images are preserved byte-for-byte as historical
evidence and excluded from fitting and clean validation aggregates. The first
47/47 holdout had 16 neighbouring pixels in unsupported U+2032's expanded crop;
its replacement has generous isolated cells. The other holdouts and the two
extra-symbol censuses passed the expanded-crop check. The extra censuses are
superseded to give every training code point the same eight-profile exposure.
Validation and legacy-validation sizes must not be used to fit the generated font.
Schema version 2 preserves the original keys and adds `role`, `anchor`,
`orientation`, optional case `width`/`height`, and cell `anchorX`/`anchorY`.
The latter are the actual ZPL command coordinates; `x`/`y` are crop rectangles.
Old cases default to FT/N and the manifest's 2436×2436 size.

Labelary explicitly warns that Font 0 cannot display U+037E, U+2044, U+20BD,
U+2206, U+220F, U+2211, U+221A, U+221E, U+2248, U+2260, U+2264, U+2265,
U+2713, U+2717, U+FB01, and U+FB02. Their reference cells are empty. They
remain in the tests. The expanded census identifies another 38 unavailable
code points, for 54 in total, listed in `unsupportedCharacters`.
QtZpl must emit `font-glyph-missing` with the code point
and source position. Silently dropping or replacing them does not pass.

Labelary limits `X-Warnings` to 20 entries. The two extra-symbol census pages
use opposite code-point order so the collected warnings cover both ends of
the census. `oracleReportedUnsupportedCharacters` preserves only the warnings
from that response; `unsupportedCharacters` also includes independently
confirmed missing characters present on that page. Empty cells are checked
against the warning evidence. An unreported blank is never called supported.
U+0020 SPACE and U+00A0 NO-BREAK SPACE are intentionally blank supported glyphs.

`qtzpl_font_glyphs_tests` compares every cell and the entire page. It writes
`font-glyph-results.json` next to its executable, with per-glyph differences,
ink bounds, diagnostics, and all failed glyph/size combinations. Failed page
rasters are also written in the build directory, never over these references.

`metrics-manifest.json` describes two separate advance-ruler pages at 30/100,
FT/N. Each field repeats one character three times and adds `|`; a standalone
`|` on the same page supplies its bearing. `metrics.json` records the last
marker's measured displacement divided by three, and the advance estimate
divided by the nominal ZPL font width. These are measured estimates with a
one-dot displacement uncertainty (1/300 nominal width unit), not font-file
units-per-em metrics. Missing characters remain explicitly unsupported. Ruler
pages are not single-glyph training or validation cases.

`semantics-manifest.json` describes a separate seven-field probe of literal
and hex-escaped backslash, cent, slash, yen and currency sign under `^CI28`.
The first four masks are pixel-identical at 70/100: Labelary renders both
U+005C and U+00A2 as the cent-shaped glyph, irrespective of literal/hex spelling.
This records the oracle's unusual behavior; it is not a Unicode equivalence.
The semantic probe is excluded from the single-glyph atlas test and training.
Its original image and pairwise pixel observations are checked offline.

`clamp-manifest.json` describes a separate 24-field H/I/A/a probe. For every
letter, masks at height/width pairs 4/30 and 10/30, 30/4 and 30/10, and 4/4 and
10/10 are pixel-identical relative to the FT/N anchor. This independently
confirms the observed minimum of ten dots on both axes. It is not training data.

Regeneration is manual; the default invocation only prints a plan:

```text
py -3 tools/fetch_font_goldens.py --check
py -3 tools/fetch_font_goldens.py --fetch --metrics --max-requests 16
py -3 tools/fetch_font_goldens.py --semantics-only --fetch
py -3 tools/fetch_font_goldens.py --clamp-only --fetch
py -3 tools/fetch_font_goldens.py --clean-training --fetch --max-requests 16
py -3 tools/fetch_font_goldens.py --profiles 11,13,23,31,64 --anchors FO,FT --orientations N,R,I,B
py -3 tools/fetch_font_goldens.py --size-range 1:96
```

Add `--fetch` only after reviewing the plan. Every network request, including
retries, counts toward `--max-requests` (12 by default). A plan exceeding that
budget is rejected before fetching. Cached source-identical pairs require no
request. Requests are at least 1.5 seconds apart; HTTP 429 receives bounded
backoff. Sizes are bounded to 1..512 dots and large atlases paginate. Larger
size sweeps remain manual and do not imply exhaustive acceptance coverage.
`--clean-training` stages its eleven training pages and two repaired 47/47
holdout pages in `training-manifest.json`. It checks the complete stage before
atomically promoting it into `manifest.json`; failed or interrupted downloads
do not change active training roles. Cached reruns require no network requests.

The offline check verifies PNG CRCs, exact monochrome format and dimensions,
new source/image SHA-256 values, missing-glyph evidence, cell bounds, isolation
halos and expanded-crop contamination for active train/validation, and all
advance measurements. PNGs are saved directly from the response, never
thresholded or rerasterized. Normal tests perform no network calls.
