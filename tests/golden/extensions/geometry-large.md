`geometry-large.zpl` and `geometry-large-labelary-bitonal.png` were requested
from Labelary on 2026-10-07, at 8 dpmm and 8×8 inches, with `Accept: image/png`,
`X-Quality: Bitonal`, and `X-Linter: On`. The response contains no warnings;
the PNG is the original one-bit response, 1624×1624 pixels.

This regression covers filled and hollow circles up to 1000 dots, and both
diagonal directions with non-integral slopes. The thin circle is a single
ring area: its outer and inner curves have opposite directions. The curve
direction matters at boundary pixels when coordinates are quantized.
