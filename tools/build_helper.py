#!/usr/bin/env python3
"""Build QtZpl with a correctly initialized Qt/MSVC environment.

Examples:
    py -3 tools/build_helper.py all --clean
    py -3 tools/build_helper.py test --config Debug
    py -3 tools/build_helper.py build --target QtZpl
    py -3 tools/build_helper.py install --install-prefix C:\\QtZpl\\RelWithDebInfo
"""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


PROJECT_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_BUILD_DIR = PROJECT_ROOT / "build_agent"
QT_ROOT = Path(os.environ.get("QT_ROOT", "C:/Qt"))
QT_TOOLS = QT_ROOT / "Tools"


def find_vcvars64() -> Path:
    for edition in ("Community", "Professional", "Enterprise", "BuildTools"):
        candidate = Path(
            f"C:/Program Files/Microsoft Visual Studio/2022/{edition}"
            "/VC/Auxiliary/Build/vcvars64.bat"
        )
        if candidate.is_file():
            return candidate
    raise FileNotFoundError("Visual Studio 2022 vcvars64.bat was not found")


def capture_environment(qt_dir: Path, compiler: str) -> dict[str, str]:
    qtenv = qt_dir / "bin" / "qtenv2.bat"
    if not qtenv.is_file():
        raise FileNotFoundError(f"Qt environment script was not found: {qtenv}")

    fd, capture_name = tempfile.mkstemp(prefix="qtzpl_env_", suffix=".txt")
    os.close(fd)
    capture = Path(capture_name)
    commands = [f'call "{qtenv}"']
    if compiler.lower().startswith("msvc"):
        commands.append(f'call "{find_vcvars64()}"')
    commands.append(f'set > "{capture}"')

    try:
        completed = subprocess.run(
            " && ".join(commands), shell=True, text=True, encoding="cp866",
            errors="replace", capture_output=True, check=False,
        )
        if completed.returncode != 0:
            raise RuntimeError(
                "Failed to initialize Qt/compiler environment\n"
                f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
            )
        environment = os.environ.copy()
        for line in capture.read_text(encoding="cp866", errors="replace").splitlines():
            if "=" in line:
                name, value = line.split("=", 1)
                environment[name] = value
    finally:
        capture.unlink(missing_ok=True)

    tool_paths = [QT_TOOLS / "CMake_64" / "bin", QT_TOOLS / "Ninja"]
    path_keys = [name for name in environment if name.casefold() == "path"]
    current_path = environment[path_keys[-1]] if path_keys else ""
    for name in path_keys:
        del environment[name]
    environment["PATH"] = current_path + os.pathsep + os.pathsep.join(map(str, tool_paths))
    environment["QTDIR"] = str(qt_dir)
    return environment


def cached_compiler(build_dir: Path) -> Path | None:
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        return None
    for line in cache.read_text(encoding="utf-8", errors="ignore").splitlines():
        if line.startswith("CMAKE_CXX_COMPILER:FILEPATH="):
            return Path(line.split("=", 1)[1].strip())
    return None


def run(command: list[str], environment: dict[str, str]) -> None:
    print("+", subprocess.list2cmdline(command), flush=True)
    completed = subprocess.run(command, cwd=PROJECT_ROOT, env=environment, check=False)
    if completed.returncode != 0:
        raise SystemExit(completed.returncode)


def configure(args: argparse.Namespace, environment: dict[str, str], qt_dir: Path) -> None:
    qt_cmake = qt_dir / "bin" / "qt-cmake.bat"
    if not qt_cmake.exists():
        qt_cmake = qt_dir / "bin" / "qt-cmake"
    command = [
        str(qt_cmake), "-S", str(PROJECT_ROOT), "-B", str(args.build_dir),
        "-G", "Ninja", f"-DCMAKE_BUILD_TYPE={args.config}",
        f"-DQTZPL_BUILD_TESTS={'ON' if args.tests else 'OFF'}",
        f"-DQTZPL_BUILD_EXAMPLES={'ON' if args.examples else 'OFF'}",
        f"-DQTZPL_BUILD_SHARED={'ON' if args.shared else 'OFF'}",
    ]
    if args.install_prefix is not None:
        command.append(f"-DCMAKE_INSTALL_PREFIX={args.install_prefix}")
    run(command, environment)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", nargs="?", default="all", choices=("configure", "build", "test", "install", "gallery", "all"))
    parser.add_argument("--qt-version", default="6.11.1")
    parser.add_argument("--compiler", default="msvc2022_64")
    parser.add_argument("--config", default="RelWithDebInfo", choices=("Debug", "Release", "RelWithDebInfo"))
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD_DIR)
    parser.add_argument(
        "--install-prefix",
        type=Path,
        default=os.environ.get("QTZPL_ROOT"),
        help="Install prefix (defaults to the QTZPL_ROOT environment variable)",
    )
    parser.add_argument("--target", default="all")
    parser.add_argument("--parallel", type=int, default=4)
    parser.add_argument("--output-dir", type=Path, default=PROJECT_ROOT / "examples" / "rendered")
    parser.add_argument("--clean", action="store_true")
    parser.add_argument("--static", dest="shared", action="store_false")
    parser.add_argument("--no-tests", dest="tests", action="store_false")
    parser.add_argument("--no-examples", dest="examples", action="store_false")
    parser.set_defaults(shared=True, tests=True, examples=True)
    args = parser.parse_args()
    args.build_dir = args.build_dir.resolve()
    if args.install_prefix is not None:
        args.install_prefix = args.install_prefix.resolve()
    if args.action == "install" and args.install_prefix is None:
        parser.error("install requires --install-prefix or the QTZPL_ROOT environment variable")

    qt_dir = QT_ROOT / args.qt_version / args.compiler
    if not qt_dir.is_dir():
        parser.error(f"Qt kit does not exist: {qt_dir}")

    if args.clean and args.build_dir.exists():
        print(f"Removing {args.build_dir}")
        shutil.rmtree(args.build_dir)
    compiler = cached_compiler(args.build_dir)
    if compiler and not compiler.exists():
        print(f"Removing stale build directory (missing compiler: {compiler})")
        shutil.rmtree(args.build_dir)

    environment = capture_environment(qt_dir, args.compiler)
    if args.action in ("configure", "all") or not (args.build_dir / "CMakeCache.txt").exists():
        configure(args, environment, qt_dir)
    if args.action == "configure":
        return 0

    cmake = str(QT_TOOLS / "CMake_64" / "bin" / "cmake.exe")
    target = "install" if args.action == "install" else ("qtzpl_gallery" if args.action == "gallery" else args.target)
    run([cmake, "--build", str(args.build_dir), "--target", target, "--parallel", str(args.parallel)], environment)
    if args.action in ("test", "all") and args.tests:
        # The shared QtZpl DLL is emitted at the build root while the test
        # executable lives in build_agent/tests. Make it discoverable without
        # copying artifacts or modifying the user's global PATH.
        environment["PATH"] = str(args.build_dir) + os.pathsep + environment.get("PATH", "")
        ctest = str(QT_TOOLS / "CMake_64" / "bin" / "ctest.exe")
        run([ctest, "--test-dir", str(args.build_dir), "--output-on-failure", "-C", args.config], environment)
    if args.action == "gallery":
        environment["PATH"] = str(args.build_dir) + os.pathsep + environment.get("PATH", "")
        run([str(args.build_dir / "examples" / "qtzpl_gallery.exe"), str(args.output_dir.resolve())], environment)

    print(f"QtZpl {args.action} succeeded. Build directory: {args.build_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
