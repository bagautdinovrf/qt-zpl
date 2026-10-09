# Font 0 ASCII-hyphen research and validation

These original Labelary bitonal pages contain only ASCII hyphens (`-`,
U+002D) and leading spaces (U+0020). The first two pages were initially planned
as independent validation, with scenarios fixed before acquisition. Their
observed phase differences subsequently informed the grid-fitting rule, so
they now serve as research and regression cases, not untouched holdouts.
A third page probes the quantization boundary. Only `fresh-holdout`, acquired
after model selection, is the final untouched validation page (see below).
There are no visible titles, letters, rulers or graphic markers, so the entire
output page can be compared pixel-for-pixel.

| Fixture | Dots | Fields | Coverage |
|---|---:|---:|---|
| `small-matrix` | 2560×2560 | 64 | h/w 13/17, 17/13, 23/29, 29/23, 37/61, 61/37, 25/25, 25/37; each FO/FT × N/R/I/B |
| `large-and-phases` | 2800×2800 | 48 | h/w 113/113, 257/257, 113/257, 257/113 × FO/FT × N/R/I/B; 16 extra h25/w25 phase cases |
| `boundary-probe` | 2800×2800 | 64 | Ten independent h/w boundary pairs × FO/FT N × zero/one leading space; R/I/B extensions for 102/25 and 25/102 |

Small cases use zero through three leading spaces, one through three hyphens,
and `^FPH` spacing 0, 1, 2 or 3. Large cases use exactly one visible hyphen and
at most one leading space. The extra h25/w25 cases pair FO/FT with each of
zero, one, two and three leading spaces, either one hyphen with no spacing or
three hyphens with spacing 2. Leading spaces intentionally produce fractional
pen coordinates from the proportional font's space advance; field anchors
remain integer printer dots. Width and height are varied independently.

The boundary pairs are 25/257, 257/25, 101/25, 102/25, 25/101, 25/102,
101/101, 102/102, 100/100 and 103/103. Each uses exactly one visible hyphen,
spacing zero, FO/FT and zero/one leading space. The 102/25 and 25/102 pairs
add both anchors and phases in R/I/B to distinguish width, height and rotation
effects around the suspected quantization transition. The manifest explicitly
marks this page `quantization-boundary-research`; the first two retain their
historical `independent-validation` acquisition labels. Those labels record
the original plan, not the final role of data used during model refinement.

Each field occupies a separate square cell. The acquisition tool checks the
original ink bounds, minimum four-dot cell clearance, nonempty content and
absence of ink outside the assigned cells. Observed per-cell bounds and ink
counts are diagnostic metadata, not acceptance tolerances. Acceptance is an
exact whole-page comparison, including unused white cells and spaces.

`manifest.json` freezes the scenario and ZPL hashes before the first request,
and records all request URLs, headers, dates, warnings and response hashes.
Its `layoutHistory` records one initial isolation repair: centered fields
crossed cell edges, so only their integer anchors were relocated. The first
two pages' h/w, text, fractional phases and spacing were unchanged; their
previous plan and source hashes are retained. This changes page placement,
not the glyph scenarios or renderer parameters at acquisition time.
Requests use `X-Quality: Bitonal`, 8 dpmm, and `(dots + 0.25) / 203` inches;
all dimensions stay below 15 inches. PNG bytes are saved unchanged after
validating the signature, dimensions, 1-bit grayscale IHDR and every chunk
CRC. No PDF font data, copied contours, thresholded images or resampled images
are used.

Routine offline validation of both fixture sets from the repository root:

```text
py -3 tests/golden/font0-dashes/check.py
```

This read-only checker verifies both frozen scenario hashes, original ZPL and
PNG bytes, request provenance, field isolation and the fresh holdout's hashes
of prior acquisition files. It neither rewrites manifests nor creates Python
bytecode caches. Normal library tests embed these saved ZPL/PNG resources and
never call the checker or the network. Only Python's standard library is needed.

`fetch.py` and `fetch_fresh_holdout.py` are archived acquisition scripts;
preserve their original bytes. Their historical `--check` paths also rewrite
manifests and can change line endings across platforms, so they are not the
routine validation command. `check.py` imports their scenario and validation
functions without running either acquisition entry point.

## Fresh validation after model selection

`fresh-holdout.zpl` and its original `fresh-holdout-labelary-bitonal.png`
form a separate 2800×2800 page of 48 fields. Its independent
`fresh-holdout-manifest.json` was frozen before acquisition, after the dash
model had been selected. This page must not be used to tune that model.
The six new h/w pairs are 33/71, 77/19, 99/53, 127/31, 219/83 and 347/41;
every pair covers FO/FT × N/R/I/B. The first four use `^FPH` with zero through
three leading spaces, one through three hyphens and spacing zero through
three. The 219/83 pair uses `^FPR` and the 347/41 pair uses `^FPV`; these
direction checks have one hyphen, no spaces and spacing zero through three.

Each field has a 400-dot isolation cell in a 7×7 grid. Before acquisition,
integer anchors were chosen by centering conservative design rectangles
`x = [-0.05w, w]`, `y = [-h, 0]` around each visible glyph, using only the
previously measured advance and FO/FT anchor metrics. These rectangles do
not use the candidate dash contours or raster quantization. The recorded
plan was accepted on its first request without any geometry changes.
The same original-byte PNG and isolation checks apply. The separate manifest
also records hashes of the original three-page sources, PNGs, manifest and
acquisition script; all remain unchanged.

The same read-only `check.py` command above validates this fresh page together
with the original three pages. The archived acquisition scripts retain the
historical process; daily checks must not rewrite these frozen artifacts.
