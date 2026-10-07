# Remaining DataMatrix encodation differences

These are diagnostic inputs, not accepted raster output. PNGs were requested
directly from Labelary with `X-Quality: Bitonal`, 8 dpmm, 2 x 1 inches, on
2026-10-07; each has its exact ZPL input. The production C40/Text extension is
deliberately limited to cases where compaction permits a smaller symbol or
satisfies a requested size. Existing ASCII/GS1 module vectors remain unchanged.

| Input | Actual size | Remaining difference |
| --- | --- | --- |
| `dm-compact-ascii-json` | 24 x 24 modules | Labelary chooses different codewords at the same symbol capacity; the library retains the mandatory existing ASCII module vector. 1809 differing raster pixels at module size 3. |
| `dm-compact-text-terminal-two` (`abcdefgh`) | 14 x 14 modules | Labelary chooses Text even though ASCII fits the same size. 693 differing pixels. |
| `dm-compact-c40-punctuation` (`ABC-DEF/GHI.JKL`) | 18 x 18 modules | Mode selection differs; exact remaining encoding mode has not yet been established. 1161 differing pixels. |

All three symbols still encode the original bytes. These differences require
further mode-selection work; full DataMatrix pixel parity is not claimed.
Do not lower the existing strict corpus or module-vector acceptance checks.
