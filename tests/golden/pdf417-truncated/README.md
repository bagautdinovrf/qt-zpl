# Truncated PDF417 Labelary references

These four unmodified bitonal PNGs were obtained from Labelary on 2026-10-07
using `POST https://api.labelary.com/v1/printers/8dpmm/labels/2x1/0/` with
`Accept: image/png`, `X-Quality: Bitonal`, and `X-Linter: On`. Each image is
406 x 203 dots, with one-bit sample depth, and has matching ZPL input.

The fixtures use ^B7 with security level 2, two columns, eight rows, three-dot
row height, truncation enabled, and all four orientations. Labelary's only
warning is that the deliberately one-dot module width may affect scanning.
The tests compare every pixel and do not use the network.
