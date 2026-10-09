#!/usr/bin/env python3
"""Build QtZpl with a correctly initialized Qt/MSVC environment.

Examples:
    py -3 tools/build_helper.py
    py -3 tools/build_helper.py all --clean
    py -3 tools/build_helper.py test --config Debug
    py -3 tools/build_helper.py build --target QtZpl
    py -3 tools/build_helper.py benchmark --config Release
    py -3 tools/build_helper.py install --install-prefix C:\\QtZpl\\Release
"""

from __future__ import annotations

import argparse
import copy
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
from uuid import uuid4


PROJECT_ROOT = Path(__file__).resolve().parent.parent
BUILD_ROOT = PROJECT_ROOT / "build_agent"
QT_ROOT = Path(os.environ.get("QT_ROOT", "C:/Qt"))
QT_TOOLS = QT_ROOT / "Tools"

ALL_BUILD_CONFIGS: tuple[tuple[str, Path], ...] = (
    ("Debug", BUILD_ROOT / "build_agent_debug"),
    ("Release", BUILD_ROOT / "build_agent_release"),
)


def build_dir_for_config(config: str) -> Path:
    return BUILD_ROOT / f"build_agent_{config.casefold()}"


def is_directory_link(path: Path) -> bool:
    """Detect symlinks and Windows reparse points, including junctions."""
    if path.is_symlink():
        return True
    try:
        attributes = getattr(path.lstat(), "st_file_attributes", 0)
    except FileNotFoundError:
        return False
    return bool(attributes & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400))


def checked_build_dir(path: Path) -> Path:
    """Only direct, non-redirected build_* children may contain build trees."""
    path = path.absolute()
    resolved = path.resolve(strict=False)
    if (BUILD_ROOT.resolve(strict=False) != BUILD_ROOT
            or resolved.parent != BUILD_ROOT or not resolved.name.startswith("build_")
            or resolved.name == "build_" or ".." in path.parts
            or any(is_directory_link(part) for part in (path, *path.parents))):
        raise ValueError(f"Build directory must be a direct build_* child of {BUILD_ROOT}: {path}")
    if resolved.exists() and not resolved.is_dir():
        raise ValueError(f"Build directory is not a directory: {resolved}")
    return resolved


def build_cache_values(build_dir: Path) -> dict[str, str]:
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        return {}
    values = {}
    for line in cache.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line or line.startswith(("#", "//")):
            continue
        name, separator, value = line.partition("=")
        if separator:
            values[name.partition(":")[0]] = value.strip()
    return values


def cache_needs_refresh(build_dir: Path) -> bool:
    values = build_cache_values(build_dir)
    previous_dir = values.get("CMAKE_CACHEFILE_DIR")
    compiler = values.get("CMAKE_CXX_COMPILER")
    return bool((previous_dir and Path(previous_dir).resolve() != build_dir)
                or (compiler and not Path(compiler).exists()))


def checked_cache_owner(build_dir: Path) -> None:
    """Do not clean or refresh an unrelated cache or linked CMake metadata."""
    for name in ("CMakeCache.txt", "CMakeFiles"):
        if is_directory_link(build_dir / name):
            raise ValueError(f"Refusing linked CMake metadata: {build_dir / name}")
    values = build_cache_values(build_dir)
    source = values.get("CMAKE_HOME_DIRECTORY")
    if not source or Path(source).resolve() != PROJECT_ROOT or values.get("CMAKE_PROJECT_NAME") != "QtZpl":
        raise ValueError(f"Refusing to remove or refresh an unrecognized QtZpl build: {build_dir}")


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

    environment_marker = f"__AGENT_ENV_CAPTURE_{uuid4().hex}__"
    commands = [f'call "{qtenv}"']
    if compiler.lower().startswith("msvc"):
        commands.append(f'call "{find_vcvars64()}"')
    commands.extend(["echo(", f"echo {environment_marker}", "set"])

    completed = subprocess.run(
        " && ".join(commands), shell=True, text=True, encoding="cp866",
        errors="replace", capture_output=True, check=False,
    )
    lines = completed.stdout.splitlines(keepends=True)
    marker_index = next(
        (index for index, line in enumerate(lines) if line.strip() == environment_marker),
        None,
    )
    if completed.returncode != 0 or marker_index is None:
        diagnostic_stdout = "".join(lines if marker_index is None else lines[:marker_index])
        raise RuntimeError(
            "Failed to initialize Qt/compiler environment\n"
            f"stdout:\n{diagnostic_stdout}\nstderr:\n{completed.stderr}"
        )
    environment = os.environ.copy()
    for line in lines[marker_index + 1:]:
        name, separator, value = line.rstrip("\r\n").partition("=")
        if name and separator:
            environment[name] = value

    tool_paths = [QT_TOOLS / "CMake_64" / "bin", QT_TOOLS / "Ninja"]
    path_keys = [name for name in environment if name.casefold() == "path"]
    current_path = environment[path_keys[-1]] if path_keys else ""
    for name in path_keys:
        del environment[name]
    environment["PATH"] = current_path + os.pathsep + os.pathsep.join(map(str, tool_paths))
    environment["QTDIR"] = str(qt_dir)
    return environment


def cached_compiler(build_dir: Path) -> Path | None:
    compiler = build_cache_values(build_dir).get("CMAKE_CXX_COMPILER")
    return Path(compiler) if compiler else None


def run(command: list[str], environment: dict[str, str]) -> None:
    print("+", subprocess.list2cmdline(command), flush=True)
    completed = subprocess.run(command, cwd=PROJECT_ROOT, env=environment, check=False)
    if completed.returncode != 0:
        raise SystemExit(completed.returncode)


def configure(args: argparse.Namespace, environment: dict[str, str], qt_dir: Path) -> None:
    args.build_dir = checked_build_dir(args.build_dir)
    fresh = []
    if (args.build_dir / "CMakeCache.txt").exists():
        checked_cache_owner(args.build_dir)
    if cache_needs_refresh(args.build_dir):
        # Moved caches hold absolute binary paths. --fresh removes only CMake's
        # cache/metadata, retaining diagnostic artifacts in the build directory.
        fresh = ["--fresh"]
    qt_cmake = qt_dir / "bin" / "qt-cmake.bat"
    if not qt_cmake.exists():
        qt_cmake = qt_dir / "bin" / "qt-cmake"
    command = [
        str(qt_cmake), *fresh, "-S", str(PROJECT_ROOT), "-B", str(args.build_dir),
        "-G", "Ninja", f"-DCMAKE_BUILD_TYPE={args.config}",
        f"-DQTZPL_BUILD_TESTS={'ON' if args.tests else 'OFF'}",
        f"-DQTZPL_BUILD_EXAMPLES={'ON' if args.examples else 'OFF'}",
        f"-DQTZPL_BUILD_SHARED={'ON' if args.shared else 'OFF'}",
        f"-DQTZPL_BUILD_BENCHMARKS={'ON' if args.benchmarks else 'OFF'}",
    ]
    if args.install_prefix is not None:
        command.append(f"-DCMAKE_INSTALL_PREFIX={args.install_prefix}")
    run(command, environment)


def prepare_build_dir(args: argparse.Namespace) -> None:
    args.build_dir = checked_build_dir(args.build_dir)
    if args.clean and args.build_dir.exists():
        if any(args.build_dir.iterdir()):
            checked_cache_owner(args.build_dir)
        # Revalidate immediately before recursive deletion, even when the
        # command-line argument was checked before toolchain initialization.
        safe_directory = checked_build_dir(args.build_dir)
        print(f"Removing generated build directory: {safe_directory}", flush=True)
        shutil.rmtree(safe_directory)


def run_action(args: argparse.Namespace, environment: dict[str, str], qt_dir: Path) -> None:
    prepare_build_dir(args)
    if (args.action in ("configure", "all", "benchmark") or args.benchmarks
            or cache_needs_refresh(args.build_dir)
            or not (args.build_dir / "CMakeCache.txt").exists()):
        configure(args, environment, qt_dir)
    if args.action == "configure":
        return

    cmake = str(QT_TOOLS / "CMake_64" / "bin" / "cmake.exe")
    target = "install" if args.action == "install" else ("qtzpl_gallery" if args.action == "gallery" else args.target)
    if args.action == "benchmark":
        target = "qtzpl_benchmarks"
    run([cmake, "--build", str(args.build_dir), "--target", target, "--parallel", str(args.parallel)], environment)
    if args.action in ("test", "all") and args.tests:
        environment["PATH"] = str(args.build_dir) + os.pathsep + environment.get("PATH", "")
        ctest = str(QT_TOOLS / "CMake_64" / "bin" / "ctest.exe")
        run([ctest, "--test-dir", str(args.build_dir), "--output-on-failure", "-C", args.config], environment)
    if args.action == "gallery":
        environment["PATH"] = str(args.build_dir) + os.pathsep + environment.get("PATH", "")
        run([str(args.build_dir / "examples" / "qtzpl_gallery.exe"), str(args.output_dir.resolve())], environment)

    if args.action == "benchmark":
        environment["PATH"] = str(args.build_dir) + os.pathsep + environment.get("PATH", "")
        command = [str(args.build_dir / "benchmarks" / "qtzpl_benchmarks.exe"),
                   "--samples", str(args.benchmark_samples),
                   "--min-ms", str(args.benchmark_min_ms),
                   "--output", str(args.benchmark_output or args.build_dir / "benchmark-results.json")]
        if args.benchmark_filter:
            command.extend(["--filter", args.benchmark_filter])
        run(command, environment)

    print(f"QtZpl {args.action} ({args.config}) succeeded. Build directory: {args.build_dir}")


def wants_all_configs(argv: list[str], action: str) -> bool:
    if action in ("install", "gallery", "configure", "test", "benchmark"):
        return False
    return not any(argument == option or argument.startswith(option + "=")
                   for argument in argv for option in ("--config", "--build-dir"))


def resolve_build_dir(config: str, build_dir: Path | None) -> Path:
    path = build_dir if build_dir is not None else build_dir_for_config(config)
    if not path.is_absolute():
        path = (BUILD_ROOT / path if len(path.parts) == 1 and path.name.startswith("build_")
                and path.name != "build_agent"
                else PROJECT_ROOT / path)
    return checked_build_dir(path)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", nargs="?", default="all", choices=("configure", "build", "test", "install", "gallery", "benchmark", "all"))
    parser.add_argument("--qt-version", default="6.11.2")
    parser.add_argument("--compiler", default="msvc2022_64")
    parser.add_argument("--config", choices=("Debug", "Release", "RelWithDebInfo"))
    parser.add_argument("--build-dir", type=Path,
                        help="Direct build_* child of build_agent; a bare build_* name is placed there")
    parser.add_argument(
        "--install-prefix",
        type=Path,
        default=os.environ.get("QTZPL_ROOT"),
        help="Install prefix (defaults to the QTZPL_ROOT environment variable)",
    )
    parser.add_argument("--target", default="all")
    parser.add_argument("--parallel", type=int, default=4)
    parser.add_argument("--output-dir", type=Path, default=BUILD_ROOT / "rendered")
    parser.add_argument("--clean", action="store_true")
    parser.add_argument("--static", dest="shared", action="store_false")
    parser.add_argument("--no-tests", dest="tests", action="store_false")
    parser.add_argument("--no-examples", dest="examples", action="store_false")
    parser.add_argument("--benchmarks", action="store_true", help="Build optional performance benchmark executable")
    parser.add_argument("--benchmark-filter", default="", help="Run only benchmark names containing this text")
    parser.add_argument("--benchmark-samples", type=int, default=9)
    parser.add_argument("--benchmark-min-ms", type=int, default=50)
    parser.add_argument("--benchmark-output", type=Path, help="Benchmark JSON output (defaults to build directory)")
    parser.set_defaults(shared=True, tests=True, examples=True)
    args = parser.parse_args()
    if args.action == "benchmark":
        args.benchmarks = True
        if args.config is None:
            args.config = "Release"
    if args.install_prefix is not None:
        args.install_prefix = args.install_prefix.resolve()
    if args.action == "install" and args.install_prefix is None:
        parser.error("install requires --install-prefix or the QTZPL_ROOT environment variable")

    try:
        args.build_dir = resolve_build_dir(args.config or "Debug", args.build_dir)
    except ValueError as error:
        parser.error(str(error))

    qt_dir = QT_ROOT / args.qt_version / args.compiler
    if not qt_dir.is_dir():
        parser.error(f"Qt kit does not exist: {qt_dir}")

    environment = capture_environment(qt_dir, args.compiler)

    if wants_all_configs(sys.argv[1:], args.action):
        print("Building all preset configurations: Debug, Release")
        for config, _ in ALL_BUILD_CONFIGS:
            variant = copy.copy(args)
            variant.config = config
            variant.build_dir = resolve_build_dir(config, None)
            if args.action in ("test", "all"):
                variant.tests = False
            run_action(variant, environment, qt_dir)
        return 0

    variant = copy.copy(args)
    variant.config = args.config or "Debug"
    variant.build_dir = resolve_build_dir(variant.config, args.build_dir)
    run_action(variant, environment, qt_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
