#!/usr/bin/env python3
"""Fetch or verify the pinned OFL Font 0 research sources.

This maintenance tool is never run by the library, CMake, or normal tests.
Existing correct files are reused without a network request. Font binaries stay
unmodified; derivative generation belongs to a separate tool.
Authored sources are verified locally, including their generator's checksum;
missing authored assets require explicit local regeneration, never a download.

Examples:
    py -3 tools/fetch_font0_sources.py
    py -3 tools/fetch_font0_sources.py --verify-only --check-fonts
    py -3 tools/fetch_font0_sources.py --fonts roboto-condensed noto-sans

Only --check-fonts requires fontTools (pip install fonttools).
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import sys
import tempfile
from urllib.parse import urlsplit
from urllib.request import Request, urlopen


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_MANIFEST = ROOT / "third_party/font0-native/sources.json"
MAX_FILE_BYTES = 8 * 1024 * 1024
RUSSIAN_LETTERS = set(range(0x0410, 0x0450)) | {0x0401, 0x0451}


def font_coverage(cmap: dict[int, str]) -> dict[str, int]:
    """Counts deliberately describe encoded characters, not total glyphs."""
    encoded = set(cmap)
    return {
        "unicode_codepoints": len(encoded),
        "ascii_printable": len(encoded & set(range(0x20, 0x7F))),
        "russian_letters": len(encoded & RUSSIAN_LETTERS),
        "cyrillic_u0400_u052f": len(encoded & set(range(0x0400, 0x0530))),
        "greek_u0370_u03ff": len(encoded & set(range(0x0370, 0x0400))),
    }


def font_axes(font: object) -> list[dict[str, str | float]]:
    if "fvar" not in font:
        return []
    return [
        {"tag": axis.axisTag, "minimum": axis.minValue,
         "default": axis.defaultValue, "maximum": axis.maxValue}
        for axis in font["fvar"].axes
    ]


def source_path(base: Path, relative: str) -> Path:
    if (not isinstance(relative, str) or "\\" in relative
            or Path(relative).is_absolute()):
        raise ValueError("Source paths must use relative forward-slash paths")
    path = (base / relative).resolve()
    sources = (base / "sources").resolve()
    if not path.is_relative_to(sources) or path == sources:
        raise ValueError(f"Source path escapes the sources directory: {relative}")
    return path


def load_manifest(path: Path) -> tuple[dict, list[dict]]:
    data = json.loads(path.read_text(encoding="utf-8"))
    if data.get("schema_version") != 1:
        raise ValueError("Unsupported font source manifest schema")
    revision = data.get("revision", "")
    if not re.fullmatch(r"[0-9a-f]{40}", revision):
        raise ValueError("Manifest must pin a full Git commit")
    fonts = data.get("fonts", [])
    if not isinstance(fonts, list) or not fonts:
        raise ValueError("Manifest contains no font sources")
    ids: set[str] = set()
    paths: set[Path] = set()
    for entry in fonts:
        font_id = entry.get("id", "")
        if not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", font_id) or font_id in ids:
            raise ValueError(f"Invalid or duplicate font id: {font_id}")
        ids.add(font_id)
        if entry.get("license_spdx") != "OFL-1.1":
            raise ValueError(f"Source {font_id} is not licensed under OFL-1.1")
        origin = entry.get("origin", "upstream")
        if origin not in ("upstream", "authored"):
            raise ValueError(f"Unknown source origin for {font_id}: {origin}")
        if origin == "authored":
            relative = entry["generator_file"]
            if not isinstance(relative, str) or "\\" in relative or Path(relative).is_absolute():
                raise ValueError(f"Invalid authored generator path: {relative}")
            generator = (ROOT / relative).resolve()
            if not generator.is_relative_to((ROOT / "tools").resolve()):
                raise ValueError(f"Authored generator must stay in project tools: {relative}")
            generator_digest = entry["generator_sha256"]
            if (not isinstance(generator_digest, str)
                    or not re.fullmatch(r"[0-9a-f]{64}", generator_digest)
                    or not generator.is_file()
                    or hashlib.sha256(generator.read_bytes()).hexdigest() != generator_digest):
                raise ValueError(f"Authored generator checksum mismatch: {relative}")
            if "source_url" in entry or "license_url" in entry:
                raise ValueError(f"Authored source must not declare remote URLs: {font_id}")
        for prefix in ("", "license_"):
            destination = source_path(path.parent, entry[prefix + "file"])
            if destination in paths:
                raise ValueError(f"Duplicate destination: {destination}")
            paths.add(destination)
            digest = entry[prefix + "sha256"]
            size = entry[prefix + "size_bytes"]
            if not isinstance(digest, str) or not re.fullmatch(r"[0-9a-f]{64}", digest):
                raise ValueError(f"Invalid SHA256 for {font_id}")
            if type(size) is not int or not 0 < size <= MAX_FILE_BYTES:
                raise ValueError(f"Invalid or excessive source size for {font_id}")
            if origin == "authored":
                continue
            url = entry["source_url" if not prefix else "license_url"]
            parts = urlsplit(url)
            if (parts.scheme != "https" or parts.netloc != "raw.githubusercontent.com"
                    or not parts.path.startswith(f"/google/fonts/{revision}/ofl/")
                    or parts.query or parts.fragment):
                raise ValueError(f"Source URL is not pinned to the declared revision: {url}")
    return data, fonts


def matches(path: Path, digest: str, size: int) -> bool:
    return (path.is_file() and path.stat().st_size == size
            and hashlib.sha256(path.read_bytes()).hexdigest() == digest)


def fetch_file(path: Path, url: str, digest: str, size: int, timeout: float) -> None:
    request = Request(url, headers={"User-Agent": "QtZpl-Font0-Sources/1"})
    with urlopen(request, timeout=timeout) as response:
        if urlsplit(response.url).scheme != "https":
            raise ValueError(f"Refusing non-HTTPS redirect for {url}")
        payload = response.read(size + 1)
    if len(payload) != size:
        raise ValueError(f"Unexpected byte count for {url}: {len(payload)}, expected {size}")
    if hashlib.sha256(payload).hexdigest() != digest:
        raise ValueError(f"SHA256 mismatch for {url}; no existing file was replaced")
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(dir=path.parent, prefix=".fetch-", delete=False) as output:
            temporary = Path(output.name)
            output.write(payload)
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def check_font(entry: dict, base: Path) -> None:
    try:
        from fontTools.ttLib import TTFont
    except ImportError as error:
        raise ValueError("--check-fonts requires fontTools: pip install fonttools") from error
    with TTFont(source_path(base, entry["file"]), lazy=False) as font:
        coverage = font_coverage(font.getBestCmap() or {})
        if coverage != entry["coverage"]:
            raise ValueError(f"Coverage metadata differs for {entry['id']}: {coverage}")
        axes = font_axes(font)
        if axes != entry["axes"]:
            raise ValueError(f"Variable axes metadata differs for {entry['id']}: {axes}")
        available_axes = {axis["tag"]: axis for axis in axes}
        for tag, value in entry["default_instance"].items():
            axis = available_axes.get(tag)
            if axis is None or not axis["minimum"] <= value <= axis["maximum"]:
                raise ValueError(f"Invalid instance coordinate {tag}={value} for {entry['id']}")
    license_text = source_path(base, entry["license_file"]).read_text(encoding="utf-8-sig")
    if "SIL OPEN FONT LICENSE Version 1.1" not in " ".join(license_text.split()):
        raise ValueError(f"Expected OFL 1.1 text is missing for {entry['id']}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--manifest", type=Path, default=DEFAULT_MANIFEST)
    parser.add_argument("--fonts", nargs="+", metavar="ID", help="Select source IDs; defaults to all")
    parser.add_argument("--verify-only", action="store_true", help="Verify cache with no network or writes")
    parser.add_argument("--check-fonts", action="store_true", help="Check cmap, axes and license using fontTools")
    parser.add_argument("--list", action="store_true", help="List pinned fonts without network or writes")
    parser.add_argument("--timeout", type=float, default=30.0, help="Per-request timeout in seconds (default 30)")
    args = parser.parse_args()
    if not 0 < args.timeout <= 120:
        parser.error("--timeout must be greater than 0 and at most 120 seconds")
    try:
        manifest_path = args.manifest.resolve()
        _, entries = load_manifest(manifest_path)
        if args.fonts:
            selected = set(args.fonts)
            unknown = selected - {entry["id"] for entry in entries}
            if unknown:
                raise ValueError(f"Unknown font IDs: {', '.join(sorted(unknown))}")
            entries = [entry for entry in entries if entry["id"] in selected]
        if args.list:
            for entry in entries:
                print(f"{entry['id']}: {entry['family']}; Russian {entry['coverage']['russian_letters']}/66")
            return 0
        downloaded = 0
        verified = 0
        for entry in entries:
            for prefix in ("", "license_"):
                path = source_path(manifest_path.parent, entry[prefix + "file"])
                digest, size = entry[prefix + "sha256"], entry[prefix + "size_bytes"]
                if not matches(path, digest, size):
                    if args.verify_only:
                        raise ValueError(f"Missing or corrupt source: {path}")
                    if entry.get("origin") == "authored":
                        raise ValueError(f"Missing or corrupt authored source: {path}. "
                                         f"Regenerate locally: {entry['generator_command']}")
                    url = entry["source_url" if not prefix else "license_url"]
                    fetch_file(path, url, digest, size, args.timeout)
                    downloaded += 1
                verified += 1
            if args.check_fonts:
                check_font(entry, manifest_path.parent)
            print(f"OK {entry['id']}: {entry['size_bytes']} bytes; Russian {entry['coverage']['russian_letters']}/66")
        print(f"Verified {verified} files in {len(entries)} families; downloaded {downloaded} files.")
        return 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"font0-sources: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
