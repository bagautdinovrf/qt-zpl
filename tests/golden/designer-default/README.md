# Designer default label

`northline.zpl` is the 799 by 799 dot NORTHLINE label constructed by
`sampleDocument()` in QtZpl Designer. It contains Latin and Cyrillic Font 0
text, reverse text, rectangles, two embedded graphics, Code 128, PDF417,
GS1 DataMatrix, and the editable EAC mark.

Only the leading `^FXQTZPL_EDITOR_V2` editor metadata comment was removed from
the Designer export. Every other byte is retained. The original export hash,
source revision, request details, warnings, and PNG hashes are recorded in
`provenance.json`.

`northline-text.zpl` keeps all 26 complete Font 0 fields unchanged, in their
original order. The reverse `B1` field retains `^FR`; its separate black
backing rectangle is absent from this text-only diagnostic fixture.
`northline-graphics.zpl` keeps complete rectangle and embedded-graphic fields,
including the backing rectangle and EAC. It excludes text and all barcodes.
Both derivatives retain the original label settings and coordinates.

The matching PNGs are unmodified Labelary responses requested with
`X-Quality: Bitonal`. Each is verified as 799 by 799, 1-bit grayscale, with
valid PNG chunk CRCs. QtZpl images are never used as reference PNGs.
The linter reports the original explicit Font 0 width `0`; the input is kept
unchanged so these fixtures exercise the exact Designer export.

Refresh manually from this directory with `py -3 fetch.py`; normal tests and
benchmarks never contact Labelary. Use `--case northline-graphics` to refresh
one reference. Updating from another Designer export requires an explicit
`--designer-export path/to/sample-default.zpl --designer-commit <revision>`.

The benchmark embeds these ZPL files and measures parsed-document rendering
and parsing plus rendering. Validation and full pixel SHA-256 fingerprints
run outside timed iterations. The presence of a full-label reference does
not assert that all QtZpl text and barcode output already matches Labelary.
