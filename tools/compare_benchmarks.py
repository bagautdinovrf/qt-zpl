#!/usr/bin/env python3
"""Compare benchmark JSON files; fail if a recorded output fingerprint changes."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    parser.add_argument("--filter", default="")
    args = parser.parse_args()
    before = json.loads(args.before.read_text(encoding="utf-8"))
    after = json.loads(args.after.read_text(encoding="utf-8"))
    if before.get("schema_version") != 1 or after.get("schema_version") != 1:
        parser.error("Unsupported benchmark JSON schema")
    for key in ("qt", "build_type", "compiler", "qt_platform", "shared_library",
                "architecture", "os"):
        if before["metadata"].get(key) != after["metadata"].get(key):
            parser.error(f"Incompatible metadata: {key}")
    if (before["metadata"].get("cpu") and after["metadata"].get("cpu")
            and before["metadata"]["cpu"] != after["metadata"]["cpu"]):
        parser.error("Incompatible metadata: cpu")
    old = {row["name"]: row for row in before["results"]}
    new = {row["name"]: row for row in after["results"]}
    if len(old) != len(before["results"]) or len(new) != len(after["results"]):
        parser.error("Duplicate case names")
    selected = sorted(name for name in old.keys() & new.keys() if args.filter in name)
    if not selected:
        parser.error("No common matching cases")
    changed = []
    checked = 0
    print("| Case | Before us | After us | Speedup | Before/after MAD % |")
    print("|---|---:|---:|---:|---:|")
    for name in selected:
        left, right = old[name], new[name]
        for key in ("category", "input_bytes", "units_per_operation", "unit", "description"):
            if left.get(key) != right.get(key):
                parser.error(f"Changed workload: {name} / {key}")
        if left.get("output_fingerprint") or right.get("output_fingerprint"):
            checked += 1
            if left.get("output_fingerprint") != right.get("output_fingerprint"):
                changed.append(name)
        baseline, current = left["median_ns"], right["median_ns"]
        print(f"| {name} | {baseline / 1000:.3f} | {current / 1000:.3f} | "
              f"{baseline / current:.2f}x | "
              f"{100 * left['mad_ns'] / baseline:.1f}/{100 * right['mad_ns'] / current:.1f} |")
    print(f"\nCompared {len(selected)} cases; checked {checked} output fingerprints.")
    unmatched = old.keys() ^ new.keys()
    if unmatched:
        print(f"Cases only in one file (not compared): {', '.join(sorted(unmatched))}")
    if changed:
        print("CHANGED OUTPUT: " + ", ".join(changed))
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
