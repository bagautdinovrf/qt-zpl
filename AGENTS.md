# AGENTS.md

Instructions for AI agents working on QtZpl.

## Mission

QtZpl is a native C++ library that parses Zebra Programming Language and
renders labels locally to `QImage`. It is intended to become a production-grade
replacement for remote renderers such as Labelary. It must not require a Go
runtime or an external rendering service.

Rendering differences from a real Zebra printer, Labelary, or an accepted
golden image are defects. Do not dismiss visible differences as approximations.
When exact behavior is not yet known, preserve the input, emit a diagnostic,
add a focused test, and document the remaining discrepancy.

## Toolchain and build

- Use the latest supported C++ dialect and Qt 6.11 or newer. MSVC builds
  explicitly use `/std:c++latest`; CMake's MSVC metadata remains CXX23 for
  compatibility with CMake 3.30. Other compilers use the newest standard
  supported by the compiler and known to CMake, with C++23 as the minimum.
- Use CMake and the exported target `QtZpl::QtZpl`.
- Keep both shared and static builds working.
- On Windows, build through `tools/build_helper.py`. It initializes Qt, MSVC,
  CMake, and Ninja without relying on the agent shell environment.
- The standard verification command is:

  ```text
  py -3 tools/build_helper.py all --clean --config Debug
  ```

- For a faster repeat run after a successful configuration:

  ```text
  py -3 tools/build_helper.py test --config Debug
  ```

- Build only the library with:

  ```text
  py -3 tools/build_helper.py build --target QtZpl
  ```

- Batch implementation work before verification. Do not rebuild or rerun tests
  after every small edit. Complete a coherent set of changes first, then build
  and run the relevant tests together. Additional intermediate verification is
  appropriate only when it is needed to diagnose a failure or de-risk a
  substantial change.
- Keep all build directories and generated build artifacts under the repository's
  `build_agent/` directory. Use `build_agent/build_agent_debug` for the default
  Debug build and `build_agent/build_agent_release` for Release. Custom and static
  builds must also stay under `build_agent/`. Do not modify or depend on Qt
  Creator build directories.
- Do not commit generated build trees, DLLs, executables, CMake caches, or test
  output images. Versioned reference images under `tests/golden` are test input,
  not generated test output, and must remain committed with their matching ZPL.

## Communication

- During long-running work, give the user a brief progress update at least once
  every 30 seconds. State what is currently being done and, when useful, what
  remains; do not leave the user without visible progress while tools, builds,
  tests, or investigation are ongoing.

## Scope

The library owns:

- parsing ZPL streams into the public `Document` model;
- preserving command order and label boundaries;
- rendering every `^XA...^XZ` block to a separate `QImage`;
- deterministic text, graphics, image, and barcode rendering;
- actionable parse and render diagnostics.

The library does not own:

- a fluent ZPL builder;
- printer communication or spooler integration;
- CLI or C ABI wrappers;
- conversion of arbitrary source images into new ZPL commands;
- network calls to external rendering services.

## Architecture

- Public headers live under `include/QtZpl`. Keep the public API minimal and
  source-compatible where practical.
- Parser implementation belongs in `src/parser.cpp`. The single ZPL
  interpreter (`State` transitions, label composition, field analysis and
  geometry) and the public render/analysis functions belong in
  `src/renderer.cpp`. Its drawing primitives live beside it:
  `src/render_fonts.cpp` (built-in fonts and metrics), `src/render_text.cpp`
  (text fields, blocks, anchors), `src/render_graphics.cpp` (`^GF`) and
  `src/render_barcodes.cpp` (barcode parameter resolution and symbols).
  They share only `src/render_internal.hpp`, namespace `QtZpl::Rendering`;
  keep file-local helpers in an anonymous namespace and never mark internals
  `QTZPL_EXPORT`. The shared library hides non-exported symbols.
- `resolveBarcode` in `src/render_barcodes.cpp` is the only interpretation of
  barcode parameters, `^BY` and `^FW` for a barcode; drawing functions and
  `FieldSettings::resolvedBarcode` both use it. Do not parse barcode
  parameters again in a drawing function or consumer-facing helper.
- Field analysis and optional geometry use that same renderer interpreter;
  do not add an independent state machine for editor consumers. Bounded graphic
  decoding belongs in `src/graphic_decoder.cpp` and is shared by the public
  decoder and renderer. Public consumer contracts are documented in README.md.
- Barcode algorithms belong in isolated encoder modules. They must be testable
  independently from `QPainter` and must not depend on GUI state.
- `Document` and `Label` are read-only to library consumers. Mutation required
  during parsing stays private to the parser.
- A parsed `Document` must be safe to render concurrently. Do not add mutable
  global state, process locale dependencies, or lazy caches without proper
  synchronization.
- Coordinates and dimensions are printer dots. Do not apply display DPI,
  `devicePixelRatio`, logical DPI, or platform scaling to label geometry.
- Default label size is 812 by 1218 dots when `^PW` or
  `^LL` are absent. Default printer resolution is 203 DPI.

## Parsing rules

- Parse all labels in the stream, not only the first one.
- Preserve empty comma-separated parameters because their position changes ZPL
  semantics.
- Unknown or unsupported commands are non-fatal by default. Preserve them as
  `UnknownCommand` when requested and emit an `unsupported-command` warning.
- Return an error only when malformed input prevents safe command or format
  recovery.
- Preserve source offset and command text in diagnostics.
- `^FH` applies only to the associated field and must be reset afterward.
- Barcode commands consume the following `^FD`/`^FV` field without rendering
  the payload as ordinary text.
- Multiple `^XA...^XZ` blocks must retain their input order.

## Rendering rules

- Render with integer dot geometry. Disable antialiasing for barcode modules,
  bitmap glyphs, and printer graphics.
- Never silently substitute a different symbology or alter barcode data.
- Invalid barcode data must produce a diagnostic and no misleading symbol.
- Respect `^FO` versus baseline-based `^FT`, `^LH`, orientation, field reverse,
  clipping, and command state lifetime.
- Do not use installed system fonts as the final implementation of Zebra
  built-in fonts. Exact glyph data and Zebra scaling behavior are the target.
- Output must be deterministic across Windows and Unix systems and independent
  of installed fonts and locale.

### Barcode requirements

- Native renderers include Code 128/39/93, Codabar, EAN-13/8, UPC-A/E,
  Interleaved/Industrial 2 of 5, GS1 DataBar types 1–6, QR Model 2,
  square/rectangular DataMatrix ECC200, PDF417 (including truncated),
  MicroPDF417 modes 0–33, Aztec, and MaxiCode modes 2–6. Unsupported parsed
  barcodes emit `barcode-render-pending`; unsupported options must produce
  a specific diagnostic instead of a substitute symbol.
- Do not add a barcode runtime dependency merely to make a symbol appear.
  Prefer a reviewed native implementation with known vectors.
- Code 128 supports the default Subset B modes `N`/`B`, automatic mode `A`,
  strict numeric-pair mode `C`, Mod 103, and Zebra invocation codes `>9`, `>:`,
  `>;`, and `>0` through `>8`. Invalid subset data must emit `code128-encode`
  and render no symbol. GS1 mode `D` strips presentation parentheses/spaces,
  inserts one leading FNC1, honors `>8` separators and packs numeric pairs.
  Mode `U` and the optional UCC Mod 10 check digit (`e=Y`) are supported.
  Keep mode U's default interpretation above the bars and its Mod 10/FNC1
  vectors. Labelary ignores `e=Y` in some non-U inputs: preserve authoritative
  Mod 10 vectors and compare that case with an explicitly appended digit.
- EAN-13 accepts 12 digits and calculates the check digit, or accepts 13 digits
  and validates it. Non-digit formatting characters may be ignored only when
  this matches the reference behavior.
- EAN-13 follows the UPC/EAN-8 retail layout in `drawExtendedLinear`: guard
  bars extend 13 dots at every module width; below-bar digits are one leading
  digit and two groups of six, in Font A at `^BY1`-`^BY2` and native OCR-B
  (`RetailFont`) from `^BY3`; above-bar interpretation is one centered Font A
  string. Code 93 and 2 of 5 interpretation lines, and every line above the
  bars, share Code 128's Font A baselines (`2m+8` above, `7m+6` below the
  bars); retail digits below keep their own layout. Preserve the
  Labelary-compatible `^FO` and `^FT` anchors for orientations N, R, I, and B.
- DataMatrix uses ECC200, including padding, Reed-Solomon correction, region
  borders, and Utah/corner placement. Keep the exact 24x24 reference-vector
  test and pixel-exact Labelary rotation tests for N, R, I, and B.
- GS1 DataMatrix is selected by `^BX` format ID `1`. Emit exactly one leading
  FNC1 codeword 232. Internal ASCII GS bytes (`0x1D`) must use ordinary ASCII
  encodation (codeword 30), never FNC1. Honor the configured escape character, `x1`, and `xdNNN`
  decimal escapes, where `x` is that character.
- Test module geometry and encoded content separately. A plausible-looking
  barcode is not proof of correctness.

## C++ and Qt style

- Use RAII, value semantics, `const`, `QStringView`, and `std::expected` where
  they make ownership and failure explicit.
- Do not throw exceptions across the public API. Return the declared error type
  and attach diagnostics for recoverable conditions.
- Do not use raw owning pointers. Prefer stack values and Qt or standard smart
  pointers when dynamic ownership is necessary.
- Avoid implicit lossy conversion between Unicode text and bytes. ZPL byte
  encoding decisions must be explicit and tied to `^CI`/field semantics.
- Use `qsizetype` for Qt container/string offsets and `int` for printer-dot
  geometry after validating its range.
- Keep exported symbols intentional via `QTZPL_EXPORT`. Private encoder helpers
  should not become public API solely for convenience; test through a private
  test target or a deliberately internal interface.
- No global `using namespace` directives in public headers.
- Keep functions focused and name diagnostics with stable kebab-case codes.

## Testing and acceptance

### Labelary is the pixel oracle; go-zpl is no longer a reference

QtZpl now exceeds go-zpl (2026-10-22). Do not rely on go-zpl for rendering,
command semantics, defaults or fixes. When diagnosing rendering differences,
comparing acceptance output, or recommending fixes, **always orient on
Labelary** and the Zebra ZPL documentation.

| Source | Role | Use for |
|---|---|---|
| [Labelary](https://labelary.com/) | **Pixel oracle** | Visual acceptance, golden PNGs, diff/debug |
| Zebra ZPL Programming Guide | **Command semantics** | Parameter meaning, defaults and ranges, confirmed with Labelary |
| `tests/corpus/go-zpl-demo/` | **Frozen input fixtures** | Regression ZPL only; not a behavior or raster reference |
| QtZpl | **Implementation under test** | Actual PNG to compare against Labelary |

**Do**

- Compare QtZpl output to committed `*-labelary-bitonal.png` goldens under
  `tests/golden/`.
- Regenerate goldens from Labelary with `X-Quality: Bitonal` when fixtures or
  sizes change.
- Use `qtzpl_compare_golden` and `tests/corpus/go-zpl-demo/manifest.json`
  render options for demo fixtures.

**Do not**

- Consult go-zpl code, WASM/site render output or its behavior during
  acceptance, debugging or fixes.
- Treat the string `go-zpl` in label text (for example in `hello.zpl`) as a
  signal to compare against the go-zpl project renderer.
- Call Labelary or any website from normal tests; use committed fixtures and
  goldens only.

The directory name `tests/corpus/go-zpl-demo/` only records where that ZPL
originally came from; go-zpl never defines the expected raster or semantics.

- Every committed fixture under `tests/corpus/go-zpl-demo/` must parse and
  render without `unsupported-command`, `barcode-render-pending`, or
  render-error diagnostics, and keeps its reviewed bitonal Labelary reference
  at each size in `manifest.json`. QtZpl output must match Labelary geometry
  and visible content; a successful parse or a plausible-looking partial
  label is not acceptance.
- The corpus is frozen: do not synchronize it with the go-zpl repository or
  site. Add new coverage from Zebra documentation examples and real-printer
  samples with Labelary references. Normal tests must use committed fixtures
  and references and never call Labelary over the network.
- Every supported command needs parser coverage for normal, omitted, empty, and
  invalid parameters.
- Every rendered primitive needs pixel assertions for position, dimensions,
  orientation, clipping, and reverse behavior.
- Every barcode encoder needs authoritative vectors that verify encoded modules,
  not only image dimensions or a dark pixel.
- Add regression tests before fixing a discovered rendering discrepancy.
- Parser fuzz input must never crash, hang, read out of bounds, or allocate an
  unbounded image.
- Validate requested image dimensions before allocating a `QImage`.
- Versioned Labelary references live under `tests/golden`. Each reference PNG
  must be produced with `X-Quality: Bitonal`, must have a matching `.zpl` input,
  and must be embedded in the test target so normal tests never use the network.
  Do not replace bitonal references with grayscale PNGs or locally thresholded
  derivatives.
- `tests/corpus/go-zpl-demo/` holds the frozen ZPL fixtures and their
  `manifest.json` render options; the Labelary bitonal PNGs under
  `tests/golden/go-zpl-demo/` are their pixel oracle.
- To inspect a fixture against its Labelary golden, build and run
  `qtzpl_compare_golden`. Pass render options from `manifest.json` when the
  fixture comes from the demo corpus:

  ```text
  py -3 tools/build_helper.py build --config Debug --target qtzpl_compare_golden
  build_agent\build_agent_debug\examples\qtzpl_compare_golden.exe ^
    tests\corpus\go-zpl-demo\hello.zpl ^
    tests\golden\go-zpl-demo\hello-page-1-labelary-bitonal.png ^
    build_agent\build_agent_debug\hello-actual.png ^
    build_agent\build_agent_debug\hello-diff.png ^
    --width 812 --height 609 --dpi 203 --ignore-label-home
  ```

  The tool writes the rendered PNG, an optional red diff overlay, and prints ink
  bounds, different pixel count, and ink Jaccard. On Windows, `qtzpl_tests` may
  not emit useful stdout under CTest; use `qtzpl_compare_golden` or run
  `qtzpl_tests -o build_agent\build_agent_debug\test_results.txt -v2` when diagnosing
  failures.
- Known open discrepancy (2026-10-09): `hello.zpl` QR (`^BQN`) matches Labelary
  pixel-perfect, but Font 0 contours still differ. With manifest options
  (812×609, `ignoreLabelHome=true`), both ink bounds are `(50,49 413×206)`,
  with 764 different pixels and ink Jaccard 0.944774. The ASCII hyphen now
  matches pixel-for-pixel, guarded by `font0HyphenMatchesHelloDemoGolden` and
  the whole-page references under `tests/golden/font0-dashes`. Font 0 advances and
  FO/FT anchors now have independent ruler, rotation and continuation fixtures
  under `tests/golden/font0-metrics`, `font0-anchors` and `font0-grid-semantics`.
  Do not compensate for remaining contour differences with a global shift.
  Keep `font0TextMatchesHelloDemoGolden` and `demoCorpusGoldenParity` strict;
  see `benchmarks/NORTHLINE-RASTER-2026-10-09.md` and
  `benchmarks/FONT0-HYPHEN-2026-10-09.md` for measured limitations.
- Preserve the current EAN-13 golden matrix: module widths 1 through 5,
  interpretation above and below, and orientations N, R, I, and B. Preserve the
  DataMatrix N/R/I/B pixel-exact golden matrix.
- Coordinates are dots, so rendering the same ZPL at 203, 300, and 600 DPI must
  produce identical raster geometry when the requested image dimensions in dots
  are identical. Keep both the QtZpl invariance assertion and the corresponding
  8/12/24 dpmm bitonal Labelary references.
- Before handing off a change, run the standard clean Debug build and report the
  exact test result. For performance-sensitive changes, also run a Release or
  RelWithDebInfo build.
- Expand visual regression coverage with Zebra documentation examples and
  selected real-printer samples; compare rendered output only against Labelary
  goldens.

## References

go-zpl (`C:\GitRepos\go-zpl`) is no longer a reference: QtZpl now exceeds it.
Do not port, compare or justify parser, command or rendering behavior from its
code, WASM or site output. The Zebra ZPL Programming Guide defines command
semantics, and committed Labelary bitonal references under `tests/golden/`
define visible output. When an unknown behavior matters, fetch a Labelary
reference with a `tools/fetch_*` maintainer script, preserve the evidence in a
regression test and fix QtZpl toward the printer-correct, Labelary-reviewed
result.
