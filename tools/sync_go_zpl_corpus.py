#!/usr/bin/env python3
"""Synchronize the versioned QtZpl demo corpus from the sibling go-zpl site.

The generated fixtures are the exact byte streams selected by the web demo.
Inline JavaScript template literals are encoded as UTF-8; file-backed examples
are decoded from the base64 assets fetched by the browser.
"""

from __future__ import annotations

import argparse
import base64
import json
from pathlib import Path
import re


PROJECT_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_GO_ZPL = PROJECT_ROOT.parent / "go-zpl"
CORPUS_DIR = PROJECT_ROOT / "tests" / "corpus" / "go-zpl-demo"


def kebab_case(name: str) -> str:
    return re.sub(r"(?<!^)(?=[A-Z])", "-", name).lower()


def examples_object(source: str) -> str:
    marker = "const examples = {"
    start = source.index(marker) + len(marker)
    end = source.index("\n};", start)
    return source[start:end]


def example_blocks(source: str) -> list[tuple[str, str]]:
    body = examples_object(source)
    matches = list(re.finditer(r"(?m)^    ([A-Za-z][A-Za-z0-9]*): \{\r?$", body))
    result: list[tuple[str, str]] = []
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else len(body)
        result.append((match.group(1), body[match.end():end]))
    return result


def required_select_keys(index_html: str) -> list[str]:
    select = re.search(r'<select id="example">([\s\S]*?)</select>', index_html)
    if select is None:
        raise RuntimeError("The example selector is absent from index.html")
    return re.findall(r'<option value="([A-Za-z][A-Za-z0-9]*)">', select.group(1))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--go-zpl", type=Path, default=DEFAULT_GO_ZPL)
    args = parser.parse_args()

    app_path = args.go_zpl / "site" / "assets" / "js" / "app.js"
    index_path = args.go_zpl / "site" / "layouts" / "index.html"
    static_path = args.go_zpl / "site" / "static"
    app_source = app_path.read_text(encoding="utf-8")
    blocks = dict(example_blocks(app_source))
    selected = required_select_keys(index_path.read_text(encoding="utf-8"))
    missing = sorted(set(selected) - set(blocks))
    if missing:
        raise RuntimeError(f"Examples selected by the site are absent from app.js: {missing}")

    CORPUS_DIR.mkdir(parents=True, exist_ok=True)
    entries: list[dict[str, object]] = []
    expected_fixtures: set[Path] = set()
    for key in selected:
        block = blocks[key]
        width_match = re.search(r"\bwidth:\s*([0-9.]+)", block)
        height_match = re.search(r"\bheight:\s*([0-9.]+)", block)
        if width_match is None or height_match is None:
            raise RuntimeError(f"Example {key} has no dimensions")
        width_inches = float(width_match.group(1))
        height_inches = float(height_match.group(1))

        inline_match = re.search(r"\bzpl:\s*`([\s\S]*?)`", block)
        file_match = re.search(r"\bfile:\s*'([^']+)'", block)
        if inline_match is not None:
            payload = inline_match.group(1).replace("\r\n", "\n").encode("utf-8")
            source_kind = "inline"
            source = "site/assets/js/app.js"
        elif file_match is not None:
            relative = file_match.group(1)
            encoded = b"".join((static_path / relative).read_bytes().split())
            payload = base64.b64decode(encoded, validate=True)
            source_kind = "base64-file"
            source = f"site/static/{relative}"
        else:
            raise RuntimeError(f"Example {key} has neither zpl nor file")

        fixture_name = f"{kebab_case(key)}.zpl"
        fixture_path = CORPUS_DIR / fixture_name
        fixture_path.write_bytes(payload)
        expected_fixtures.add(fixture_path)
        pages = len(re.findall(rb"\^XA", payload, flags=re.IGNORECASE))
        entries.append({
            "key": key,
            "fixture": fixture_name,
            "sourceKind": source_kind,
            "source": source,
            "dpi": 203,
            "widthInches": width_inches,
            "heightInches": height_inches,
            "widthDots": round(width_inches * 203),
            "heightDots": round(height_inches * 203),
            "ignoreLabelHome": True,
            "pages": pages,
        })

    for stale in CORPUS_DIR.glob("*.zpl"):
        if stale not in expected_fixtures:
            stale.unlink()
    manifest = {
        "schemaVersion": 1,
        "source": "go-zpl web demo",
        "examples": entries,
    }
    (CORPUS_DIR / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8", newline="\n"
    )
    print(f"Synchronized {len(entries)} examples ({sum(int(e['pages']) for e in entries)} pages)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
