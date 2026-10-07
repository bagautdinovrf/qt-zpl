# Code 128 UCC references and the Labelary Mod 10 discrepancy

Downloaded 2026-10-07 from Labelary at 8 dpmm, 4×1 inches (812×203 dots),
with `X-Quality: Bitonal`, `Accept: image/png`, and `X-Linter: On`.
All PNGs are original 1-bit responses with matching ZPL beside them.

`ucc-u` and `ucc-u-caption` demonstrate UCC case mode U: 19 data digits
`0012345678901234567` become `00123456789012345675` with Start C and FNC1.
The encoded data codewords are 105,102,0,12,34,56,78,90,12,34,56,75;
the Mod 103 codeword is 42.

**Labelary discrepancy:** `ucc-check-default`, `ucc-check-auto`, and
`ucc-check-explicit` preserve responses for the same source with `e=Y`.
Labelary ignores this parameter: default N and automatic A encode the original
19 digits without a Mod 10 digit. With explicit Subset C, Labelary drops the
unpaired final digit and reports: `Ignored invalid character '7' at index 22`.
These three images are evidence, not expected QtZpl output for `e=Y`.

Zebra's official ^BC documentation states that `e=Y` enables Mod 10 in addition
to the mandatory Mod 103. QtZpl follows that documented behavior. Tests verify
its codewords and compare against `ucc-u` (the same correct data encoded by
Labelary's working U mode); they do not claim parity with Labelary's broken
`e=Y` path. See https://docs.zebra.com/us/en/printers/software/zpl-pg/c-zpl-zpl-commands/r-zpl-bc.html.

The encoder rejects nonnumeric Mod 10 input and requires exactly 19 digits in
U mode, preserving invalid input through a diagnostic instead of silently
padding or truncating it.

`code128-font-a-caption` uses 4×3 inches (812×609 dots) and reproduces the
old clipping regression's `{{00}}` payload. It proves that the default ^BY5
interpretation and standalone ^AAN,50,50 have different glyph dimensions.
The clipping regression compares the complete barcode caption to this PNG
instead of incorrectly equating the two ink counts.

`caption-modules` uses 8×3 inches (1624×609 dots), covering ^BY widths 1–5
and captions above/below. Labelary warned that the two one-dot symbols should
use a larger module for scanner reliability. The default interpretation uses
Font A at 9 times the module width, fractional advance 1365/2048 em, with
baseline 7×module+6 dots below the bars or 2×module+8 dots above them.

`caption-origins-rotations` uses 8×6 inches (1624×1218 dots) and covers
N/R/I/B with default Font A captions below the bars, once with `^FO` and
once with `^FT`. `^FO` fixes the rotated bars' top-left corner; `^FT` fixes
the rotated bottom-left cell edge of the unrotated bars. Caption contours
must be rotated before monochrome rasterization: rotating an already
rasterized normal glyph changes directional dropout pixels. Retrieved with
the same bitonal headers on 2026-10-07; Labelary returned no warnings.
