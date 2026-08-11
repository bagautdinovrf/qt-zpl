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

- Use C++23 and Qt 6.11 or newer.
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

- Use the isolated `build_agent_debug` directory for the default Debug build. Do not modify or depend on Qt
  Creator build directories.
- Do not commit generated build trees, DLLs, executables, CMake caches, or test
  output images. Versioned reference images under `tests/golden` are test input,
  not generated test output, and must remain committed with their matching ZPL.

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
- Parser implementation belongs in `src/parser.cpp`; raster composition and
  ZPL rendering state belong in `src/renderer.cpp`.
- Barcode algorithms belong in isolated encoder modules. They must be testable
  independently from `QPainter` and must not depend on GUI state.
- `Document` and `Label` are read-only to library consumers. Mutation required
  during parsing stays private to the parser.
- A parsed `Document` must be safe to render concurrently. Do not add mutable
  global state, process locale dependencies, or lazy caches without proper
  synchronization.
- Coordinates and dimensions are printer dots. Do not apply display DPI,
  `devicePixelRatio`, logical DPI, or platform scaling to label geometry.
- Default label size matches the Go reference: 812 by 1218 dots when `^PW` or
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

- Current native renderers include Code 128 (`^BC`), Code 39 (`^B3`), EAN-13
  (`^BE`), and square DataMatrix ECC200 (`^BX`). Unsupported parsed barcodes
  currently emit `barcode-render-pending`.
- Do not add a barcode runtime dependency merely to make a symbol appear.
  Prefer a reviewed native implementation with known vectors.
- Code 128 supports the default Subset B modes `N`/`B`, automatic mode `A`,
  strict numeric-pair mode `C`, Mod 103, and Zebra invocation codes `>9`, `>:`,
  `>;`, and `>0` through `>8`. Invalid subset data must emit `code128-encode`
  and render no symbol. Modes `U`/`D` and the optional UCC Mod 10 check digit
  (`e=Y`) remain unsupported; do not silently approximate them.
- EAN-13 accepts 12 digits and calculates the check digit, or accepts 13 digits
  and validates it. Non-digit formatting characters may be ignored only when
  this matches the reference behavior.
- EAN-13 human-readable interpretation uses the embedded OCR-B font. Below-bar
  interpretation is grouped as one leading digit and two groups of six;
  above-bar interpretation is one centered 13-digit string. Preserve the
  Labelary-compatible `^FO` anchor for orientations N, R, I, and B.
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

- Every example published by the sibling go-zpl demo at
  `https://stirlingmarketinggroup.github.io/go-zpl/` must parse and render
  without `unsupported-command`, `barcode-render-pending`, or render-error
  diagnostics. Treat both inline examples from
  `C:\GitRepos\go-zpl\site\assets\js\app.js` and file-backed examples under
  `C:\GitRepos\go-zpl\site\static\examples` as the required compatibility
  corpus.
- For every go-zpl demo example, keep a versioned ZPL fixture and a reviewed
  bitonal Labelary reference at each size used by the demo. QtZpl output must
  match Labelary geometry and visible content; a successful parse or a
  plausible-looking partial label is not acceptance.
- Keep the demo corpus synchronized when examples are added or changed in the
  sibling repository. Normal tests must use committed fixtures and references,
  never call either the go-zpl website or Labelary over the network.
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
- Expand visual regression coverage against go-zpl and selected real-printer
  samples; keep Labelary as a reviewed reference rather than assuming it always
  overrides authoritative Zebra behavior.

## Reference implementation

The sibling repository `C:\GitRepos\go-zpl` is the current behavioral reference
for parser and renderer parity. Port behavior deliberately; do not mechanically
translate Go idioms into C++. If the Go implementation conflicts with Zebra or
an authoritative test, preserve the evidence in a regression test and fix the
Qt implementation toward the printer-correct result.
