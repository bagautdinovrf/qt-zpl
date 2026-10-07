# Aztec Labelary references

Original bitonal responses downloaded 2026-10-07 from Labelary's ZPL API:
`https://api.labelary.com/v1/printers/8dpmm/labels/1x1/0/`.
Every request used `Accept: image/png`, `X-Quality: Bitonal`, `X-Linter: On`.
The matching ZPL is beside each image. Images are 203 by 203 dots and retain
the original 1-bit PNG encoding. These fixtures are offline test inputs.

The fixture set covers automatic compact sizing, mixed text, explicit full
size, rune, reader initialization, and raw binary data through `^FH`.
