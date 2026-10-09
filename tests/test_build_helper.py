"""Build path and cleanup safety without Qt, MSVC, CMake, or real build deletion."""

from __future__ import annotations

import argparse
import importlib.util
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch


sys.dont_write_bytecode = True
PROJECT_ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("qtzpl_build_helper", PROJECT_ROOT / "tools/build_helper.py")
build_helper = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(build_helper)


class BuildHelperTests(unittest.TestCase):
    def setUp(self):
        temporary_root = PROJECT_ROOT / "build_agent"
        if temporary_root.resolve() != temporary_root or build_helper.is_directory_link(temporary_root):
            raise RuntimeError("Test files must remain inside the project build_agent directory")
        temporary_root.mkdir(exist_ok=True)
        temporary = tempfile.TemporaryDirectory(prefix="helper-tests-", dir=temporary_root)
        self.addCleanup(temporary.cleanup)
        self.temporary = Path(temporary.name).resolve()
        self.project = self.temporary / "project"
        self.root = self.project / "build_agent"
        self.root.mkdir(parents=True)
        for name, value in (("PROJECT_ROOT", self.project), ("BUILD_ROOT", self.root)):
            replacement = patch.object(build_helper, name, value)
            replacement.start()
            self.addCleanup(replacement.stop)
        self.build = self.root / "build_agent_debug"

    def cache(self, *, previous=None, source=None, compiler=None, project="QtZpl"):
        self.build.mkdir(exist_ok=True)
        values = [
            f"CMAKE_CACHEFILE_DIR:INTERNAL={previous or self.build}",
            f"CMAKE_HOME_DIRECTORY:INTERNAL={source or self.project}",
            f"CMAKE_PROJECT_NAME:STATIC={project}",
        ]
        if compiler:
            values.append(f"CMAKE_CXX_COMPILER:FILEPATH={compiler}")
        (self.build / "CMakeCache.txt").write_text("\n".join(values) + "\n", encoding="utf-8")

    def arguments(self, **overrides):
        values = dict(build_dir=self.build, config="Debug", clean=False, tests=True,
                      examples=True, shared=True, benchmarks=False, install_prefix=None,
                      action="configure", target="QtZpl", parallel=2)
        values.update(overrides)
        return argparse.Namespace(**values)

    def test_default_and_custom_direct_children(self):
        for config in ("Debug", "Release", "RelWithDebInfo"):
            with self.subTest(config=config):
                self.assertEqual(build_helper.resolve_build_dir(config, None),
                                 self.root / f"build_agent_{config.casefold()}")
        for path in (Path("build_custom"), Path("build_agent/build_custom"), self.root / "build_custom"):
            with self.subTest(path=path):
                self.assertEqual(build_helper.resolve_build_dir("Debug", path), self.root / "build_custom")

    def test_container_escape_nested_and_nonbuild_paths_are_rejected(self):
        for path in (Path("build_agent"), self.root, self.project, self.project / "build_agent_debug",
                     self.root / "build_", self.root / "artifacts", self.root / ".." / "build_escape",
                     self.root / "build_parent" / "build_nested", self.temporary / "build_outside"):
            with self.subTest(path=path), self.assertRaises(ValueError):
                build_helper.resolve_build_dir("Debug", path)

    def test_existing_file_is_not_a_build_directory(self):
        self.build.write_text("artifact", encoding="utf-8")
        with self.assertRaises(ValueError):
            build_helper.checked_build_dir(self.build)

    def test_symlink_or_junction_in_target_or_container_is_rejected(self):
        for linked in (self.build, self.root):
            with self.subTest(linked=linked), patch.object(
                    build_helper, "is_directory_link", side_effect=lambda path: path == linked):
                with self.assertRaises(ValueError):
                    build_helper.checked_build_dir(self.build)

    def test_missing_child_under_resolved_redirected_container_is_rejected(self):
        original_resolve = Path.resolve
        redirected = self.temporary / "outside"

        def resolve(path, *args, **kwargs):
            if path == self.root:
                return redirected
            if path == self.build:
                return redirected / self.build.name
            return original_resolve(path, *args, **kwargs)

        with patch.object(Path, "resolve", resolve), self.assertRaises(ValueError):
            build_helper.checked_build_dir(self.build)

    def test_windows_reparse_attribute_is_detected_without_symlink_privilege(self):
        with patch.object(Path, "is_symlink", return_value=False), patch.object(
                Path, "lstat", return_value=SimpleNamespace(st_file_attributes=0x400)):
            self.assertTrue(build_helper.is_directory_link(self.build))

    def test_clean_removes_only_validated_project_build(self):
        self.cache()
        with patch.object(build_helper.shutil, "rmtree") as remove:
            build_helper.prepare_build_dir(self.arguments(clean=True))
        remove.assert_called_once_with(self.build)
        self.assertTrue((self.build / "CMakeCache.txt").exists())

    def test_clean_preserves_unknown_artifacts_or_foreign_project(self):
        self.build.mkdir()
        artifact = self.build / "report.json"
        artifact.write_text("{}", encoding="utf-8")
        for state in ("no-cache", "foreign-source", "foreign-project"):
            if state == "foreign-source":
                self.cache(source=self.temporary / "other-project")
            elif state == "foreign-project":
                self.cache(project="OtherProject")
            with self.subTest(state=state), patch.object(build_helper.shutil, "rmtree") as remove:
                with self.assertRaises(ValueError):
                    build_helper.prepare_build_dir(self.arguments(clean=True))
                remove.assert_not_called()
                self.assertTrue(artifact.exists())

    def test_clean_revalidates_immediately_before_removal(self):
        self.cache()
        with patch.object(build_helper, "checked_build_dir", side_effect=[self.build, ValueError("redirected")]), \
                patch.object(build_helper.shutil, "rmtree") as remove:
            with self.assertRaises(ValueError):
                build_helper.prepare_build_dir(self.arguments(clean=True))
            remove.assert_not_called()

    def test_clean_never_removes_container_or_outside_target(self):
        for target in (self.root, self.temporary / "build_outside"):
            with self.subTest(target=target), patch.object(build_helper.shutil, "rmtree") as remove:
                with self.assertRaises(ValueError):
                    build_helper.prepare_build_dir(self.arguments(build_dir=target, clean=True))
                remove.assert_not_called()

    def test_missing_build_needs_no_removal(self):
        with patch.object(build_helper.shutil, "rmtree") as remove:
            build_helper.prepare_build_dir(self.arguments(clean=True))
        remove.assert_not_called()

    def test_relocated_cache_refresh_preserves_artifacts(self):
        self.cache(previous=self.project / "build_agent_debug")
        artifact = self.build / "report.json"
        artifact.write_text("{}", encoding="utf-8")
        with patch.object(build_helper, "run") as run, patch.object(build_helper.shutil, "rmtree") as remove:
            build_helper.configure(self.arguments(), {}, self.temporary / "qt")
        self.assertIn("--fresh", run.call_args.args[0])
        remove.assert_not_called()
        self.assertTrue(artifact.exists())

    def test_current_cache_needs_no_refresh(self):
        self.cache()
        with patch.object(build_helper, "run") as run:
            build_helper.configure(self.arguments(), {}, self.temporary / "qt")
        self.assertNotIn("--fresh", run.call_args.args[0])

    def test_missing_compiler_refreshes_without_recursive_removal(self):
        self.cache(compiler=self.temporary / "missing-cl.exe")
        with patch.object(build_helper, "run") as run, patch.object(build_helper.shutil, "rmtree") as remove:
            build_helper.prepare_build_dir(self.arguments())
            build_helper.configure(self.arguments(), {}, self.temporary / "qt")
        self.assertIn("--fresh", run.call_args.args[0])
        remove.assert_not_called()

    def test_plain_build_reconfigures_relocated_cache(self):
        self.cache(previous=self.project / "build_agent_debug")
        with patch.object(build_helper, "configure") as configure, patch.object(build_helper, "run"):
            build_helper.run_action(self.arguments(action="build"), {}, self.temporary / "qt")
        configure.assert_called_once()

    def test_linked_cmake_metadata_cannot_be_cleaned_or_refreshed(self):
        self.cache(previous=self.project / "build_agent_debug")
        for name in ("CMakeCache.txt", "CMakeFiles"):
            with self.subTest(name=name), patch.object(
                    build_helper, "is_directory_link", side_effect=lambda path: path == self.build / name), \
                    patch.object(build_helper, "run") as run, patch.object(build_helper.shutil, "rmtree") as remove:
                with self.assertRaises(ValueError):
                    build_helper.configure(self.arguments(), {}, self.temporary / "qt")
                with self.assertRaises(ValueError):
                    build_helper.prepare_build_dir(self.arguments(clean=True))
                run.assert_not_called()
                remove.assert_not_called()

    def test_equals_options_do_not_select_both_presets(self):
        self.assertTrue(build_helper.wants_all_configs(["build"], "build"))
        for option in ("--config=Debug", "--build-dir=build_custom", "--config", "--build-dir"):
            with self.subTest(option=option):
                self.assertFalse(build_helper.wants_all_configs(["build", option], "build"))


if __name__ == "__main__":
    unittest.main()
