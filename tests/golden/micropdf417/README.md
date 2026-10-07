# MicroPDF417 Labelary references

These PNG files were downloaded without conversion from Labelary on 2026-10-07.
Request: `POST https://api.labelary.com/v1/printers/8dpmm/labels/2x1/0/`,
headers `Accept: image/png`, `X-Quality: Bitonal`, `X-Linter: On`.
Every response is 406 x 203 dots with a one-bit PNG sample depth. The matching
ZPL input is stored beside each image. Normal tests never call the service.

`mode-00` through `mode-33` cover all Zebra ^BF mode layouts with `ABC`, a
one-dot module width and a two-dot row height. Labelary warns that a one-dot
module width can affect scanning; this deliberate test width exposes each
individual module without resampling. It does not report invalid data.

`numeric` verifies numeric compaction; `binary` verifies seven non-ASCII input
bytes introduced through ^FH. `rotation-n/r/i/b` exercise a two-dot module
width and three-dot row height. These six references have no linter warnings.

The internal encoder tests compare complete module matrices and the renderer
tests compare full label pixels, including orientation and the ^FO anchor.
