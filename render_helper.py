#!/usr/bin/env python3
"""Render the ZPL below to one or more PNG files using the local QtZpl build."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import tempfile

from tools import build_helper


# Paste your ZPL between the triple quotes and run:
#     py -3 render_helper.py
#
# GS1 group separators must be byte 0x1D in the file, or use |d029 with ^BX...,1,| .
# A raw-string spelling like \\u001D is six literal characters, not GS.
GS = "\x1d"
ZPL = f"""^XA
^FT59,474^BXN,6,200,36,36,1,|^FD|10104607098451569215NbKxo3W6f4JW{GS}91FFD0{GS}92dGVzdDSedZGhwKRKk7I+fSyrpm1svnYcsVyn3Jo2jrI=^FS
^FT280,144^A0N,56,56^FH\\^CI28^FDGTIN: 04607098451569^FS^CI27
^FT280,225^A0N,56,56^FH\\^CI28^FDSN:    5NbKxo3W6f4JW^FS^CI27
^FT280,307^A0N,56,56^FH\\^CI28^FDDate:  04$08.2026^FS^CI27
^FT280,385^A0N,56,56^FH\\^CI28^FDDate:  18@08.2028^FS^CI27
^FT280,455^A0N,56,56^FH\\^CI28^FDLot:    case0408^FS^CI27
^FT280,510^A0N,56,56^FH\\^CI28^FDArticle: 002^FS^CI27
^XZ
""".strip()

OUTPUT_DIR = Path(__file__).resolve().parent / "rendered"
BUILD_CONFIG = "Debug"
QT_VERSION = "6.11.1"
QT_COMPILER = "msvc2022_64"


def main() -> int:
    project_root = Path(__file__).resolve().parent
    build_dir = build_helper.DEFAULT_BUILD_DIR

    build_command = [
        sys.executable,
        str(project_root / "tools" / "build_helper.py"),
        "build",
        "--target",
        "qtzpl_render",
        "--config",
        BUILD_CONFIG,
        "--qt-version",
        QT_VERSION,
        "--compiler",
        QT_COMPILER,
    ]
    if subprocess.run(build_command, cwd=project_root, check=False).returncode != 0:
        return 1

    qt_dir = build_helper.QT_ROOT / QT_VERSION / QT_COMPILER
    environment = build_helper.capture_environment(qt_dir, QT_COMPILER)
    environment["PATH"] = str(build_dir) + os.pathsep + environment.get("PATH", "")

    executable = build_dir / "examples" / "qtzpl_render.exe"
    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    temporary_path: Path | None = None
    try:
        with tempfile.NamedTemporaryFile(suffix=".zpl", delete=False) as temporary:
            temporary.write(ZPL.encode("utf-8"))
            temporary_path = Path(temporary.name)
        completed = subprocess.run(
            [str(executable), str(temporary_path), str(OUTPUT_DIR)],
            cwd=project_root,
            env=environment,
            check=False,
        )
        return completed.returncode
    finally:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)


if __name__ == "__main__":
    raise SystemExit(main())
