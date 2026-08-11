# QtZpl

Native C++23 / Qt 6.11 ZPL parser and raster renderer. The library has no Go runtime dependency.

```cpp
auto result = QtZpl::render(zpl);
if (result) {
    const QList<QImage>& labels = result->labels;
}
```

The parser is tolerant by default: unsupported commands are preserved in the public document model and reported as diagnostics.

Compatibility target: every example published by the
[go-zpl web demo](https://stirlingmarketinggroup.github.io/go-zpl/) must render
locally with the same geometry and visible content as its reviewed
[Labelary](https://labelary.com/) reference. This is an acceptance target; the
currently implemented command list below does not yet cover the full demo
corpus.

Implemented barcode rendering currently includes Code 128 (`^BC`), Code 39
(`^B3`), EAN-13 (`^BE`), and square DataMatrix ECC200 (`^BX`). Code 128 supports
the default Subset B modes, automatic mode `A`, strict numeric-pair mode `C`,
and Zebra invocation codes; modes `U`/`D` and the optional UCC Mod 10 check
digit remain explicitly unsupported. GS1 DataMatrix is selected with format ID
`1`; the configured escape character supports `x1` as FNC1 and `xdNNN` decimal
byte escapes (for example `|d029` for a GS separator).

## Build

```text
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH=C:/Qt/6.11.1/msvc2022_64
cmake --build build
ctest --test-dir build --output-on-failure
```

On Windows, use the build helper to initialize Qt and MSVC and build in an
isolated `build_agent` directory:

```text
py -3 tools/build_helper.py all --clean
py -3 tools/build_helper.py test --config Debug
py -3 tools/build_helper.py build --target QtZpl
```

Install the shared library for a consuming project by setting `QTZPL_ROOT` or
passing the prefix explicitly:

```text
set QTZPL_ROOT=C:\QtZpl\RelWithDebInfo
py -3 tools/build_helper.py install --clean --config RelWithDebInfo
py -3 tools/build_helper.py install --install-prefix C:\QtZpl\RelWithDebInfo
```

It defaults to Qt 6.11.1 with `msvc2022_64`; use `--qt-version` and
`--compiler` for another installed kit.

## Rendered examples

The `qtzpl_gallery` example generates several PNG files from real ZPL strings:

```text
py -3 tools/build_helper.py gallery
```

Generated samples are stored in `examples/rendered`.
