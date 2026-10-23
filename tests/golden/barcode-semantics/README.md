# Barcode parameter semantics references

Downloaded 2026-10-22 from Labelary at 8 dpmm, 3×2 inches (609×406 dots;
`*-by6-10` and `be-below-by5-10` use 6×3 or 6×4 inches, see `size` in
`provenance.json`), with `X-Quality: Bitonal`, `Accept: image/png` and
`X-Linter: On` by `tools/fetch_barcode_semantics_goldens.py`. Every PNG is the original 1-bit
response beside its `.zpl`; `provenance.json` keeps the linter warnings.
`qtzpl_barcode_semantics_tests` compares all of them pixel by pixel.

Confirmed semantics:

- `^BY` height 0 is below Labelary's minimum and becomes 1 dot
  (`Value 0 is less than minimum value 1; used 1 instead`). An empty `^BC`
  or `^BE` height therefore uses that one dot; there is no 100-dot default
  (`*-by0-height`, `*-by-height`). `^BQ` moves its `^FO` anchor by the same
  clamped height (`bq-by*-offset`).
- The default MicroPDF417 row height is `max(1, ^BY height / rows)` of the
  mode's fixed row count (`bf-row-height-*`).
- Lower-case `y`/`n` flags are equal to upper-case flags for every symbology
  (`*-lower-*` and their `*-upper-*` twins are identical responses).
- An omitted barcode or font orientation follows `^FW`; an explicit
  orientation, including `N`, overrides it (`fw-*`). Zebra documents the
  same rule for `^FW`. For text, the later of `^FW` and an explicit `^A`
  rotation wins, also inside one field, and the next field's font follows
  `^FW` again (`text-*-then-fw-r`, `fw-r-text-next-field`; the go-zpl FedEx
  labels write `^A0N,13,18^FWB`). A barcode takes the `^FW` in effect at its
  `^B` command; a later `^FW` in the field does not rotate it
  (`bc-*-then-fw-r`, `bx-explicit-n-then-fw-r`). `^BD` has no orientation
  parameter. `^BQ` never rotates its modules, but the `^BY`-height anchor
  margin follows the effective orientation: top for `N`, left for `B`, none
  for `R` and `I`.
- Font A interpretation lines (9m×5m at module width m) of Code 93,
  Interleaved and Industrial 2 of 5, and every line above EAN/UPC bars, use
  Code 128's baselines: `2m+8` dots above or `7m+6` below the bars, from
  `^BY1` to `^BY10` (`*-above-by*`, `*-below-by*`, `*-check`, `*-caption`).
  The check digit itself changes nothing in that layout.
- EAN-13 uses the UPC layout: guard bars 13 dots longer at every module
  width, digits below in Font A at `^BY1`-`^BY2` and in native OCR-B from
  `^BY3` (`be-below-*`, `be-rotation-*`), above in one Font A line
  (`be-above-*`, `be-lower`/`be-upper`). Labelary drops a 13th digit and
  recomputes the check digit (linter warning); a valid one draws the same
  symbol.
- Aztec size `0`, an omitted size and an invalid `150` are identical
  responses: Labelary ignores sizes outside 1-99/101-104/201-232/300 (linter
  warning) and draws the default symbol (`bo-*`).
- Codabar takes its start and stop characters from `^BK` k and l in any case
  and uses `A` for an invalid one (`bk-start-stop-*`). Field characters
  outside `0-9 - $ : / . +`, including A-D anywhere, are dropped with a
  linter warning (`bk-start-stop-in-data`, `bk-start-stop-inner`, the
  `A123A` fields of `bk-*-height`). With no data character left Labelary
  prints no barcode (`bk-no-data`); QtZpl reports `codabar-encode`.

The top-level EAN-13 references `ean-module-3`..`5`, `ocr-b` and
`ean-rotation-*` came from an earlier Labelary release that printed the
digits below the bars in Font A at `^BY3`+. They were re-fetched with their
own ZPL by `fetch_barcode_semantics_goldens.py --refresh-ean13`.

Open discrepancies kept as expected failures in the test (not semantics of
the parameters themselves):

- `fw-r-text-*`, `text-*`: orientation and anchors match; the remaining
  pixels are the known Font 0 contour differences described in `AGENTS.md`.
