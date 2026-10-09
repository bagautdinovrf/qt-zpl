# Font 0 minimum cell size

`minimum-size.zpl` contains 24 pairs of the same `HIa` text: each axis below
10 dots is compared with that axis set to 10. The matrix covers `^FO`, `^FT`,
all four orientations, and independently narrow, short, and small cells.
Each pair has identical ink in the original Labelary bitonal PNG.

The renderer must clamp both the outline scale and the nominal cell used for
positioning. Clamping the outline alone shifts rotated fields. The parser
continues to preserve the original requested dimensions.

`provenance.json` records the request, date and SHA-256 hashes; `cases.json`
records the positions and independent crop measurements. The PNG was received
directly from Labelary with `X-Quality: Bitonal`, without local thresholding.
Normal tests embed both inputs and never use the network. To refresh manually,
POST the unchanged ZPL to the URL in `provenance.json` with the recorded headers
and preserve the original PNG response; update the hashes and measurements.

`qtzpl_font0_raster_tests` checks all 24 golden pairs and the same pairs rendered
by QtZpl. It also checks sizes 1 through 9 against 10 in every orientation.
This isolates the minimum-size semantics from the remaining contour defects.
