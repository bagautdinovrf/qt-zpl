# Barcode parameter semantics references

Downloaded 2026-10-22 from Labelary at 8 dpmm, 3×2 inches (609×406 dots),
with `X-Quality: Bitonal`, `Accept: image/png` and `X-Linter: On` by
`tools/fetch_barcode_semantics_goldens.py`. Every PNG is the original 1-bit
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

Open discrepancies kept as expected failures in the test (not semantics of
the parameters themselves):

- `be-*`, `b8-*-caption`: the EAN-13/EAN-8 interpretation above the bars at
  `^BY2` is placed and sized differently from Labelary.
- `b2-*-check`, `ba-*-check`: the interpretation line with a check digit
  differs from Labelary.
- `bo-*-flags`: Labelary ignores an invalid Aztec size `0` (linter warning)
  and draws an automatic symbol; QtZpl reports an encoder error.
- `fw-r-text-*`, `text-*`: orientation and anchors match; the remaining
  pixels are the known Font 0 contour differences described in `AGENTS.md`.

Codabar fixtures carry start/stop characters in the field (`A123A`). Labelary
warns that it takes start/stop from the `^BK` parameters instead and draws the
same symbol; that difference is outside these references.
