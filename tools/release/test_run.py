"""Tests for the build cap in run.py. Run: py -m unittest discover tools/release"""

import os
import sys
import tempfile
import unittest
from unittest import mock

if sys.platform != "win32":
    raise unittest.SkipTest("run.py is Windows only and says so on import")

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")))
import run  # noqa: E402


def cache_with(*lines) -> str:
    handle = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8")
    with handle:
        handle.write("\n".join(("# This is the CMakeCache file.",) + lines) + "\n")
    return handle.name


class BuildJobsTests(unittest.TestCase):
    def jobs(self, cores, override=None):
        env = {key: value for key, value in os.environ.items() if key != "QUARRY_BUILD_JOBS"}
        if override is not None:
            env["QUARRY_BUILD_JOBS"] = override
        with mock.patch.dict(os.environ, env, clear=True), mock.patch("os.cpu_count", return_value=cores):
            return run.build_jobs()

    def test_half_the_logical_cores(self):
        self.assertEqual(self.jobs(32), 16)
        self.assertEqual(self.jobs(12), 6)
        self.assertEqual(self.jobs(5), 2)

    def test_never_less_than_one(self):
        self.assertEqual(self.jobs(1), 1)
        self.assertEqual(self.jobs(None), 1)

    def test_the_override_wins(self):
        self.assertEqual(self.jobs(32, "6"), 6)
        self.assertEqual(self.jobs(4, " 12 "), 12)

    def test_an_override_that_is_not_a_positive_number_is_ignored(self):
        for junk in ("", "0", "-4", "all", "1.5"):
            self.assertEqual(self.jobs(32, junk), 16, junk)


class ParallelArgsTests(unittest.TestCase):
    def setUp(self):
        self.made = []

    def tearDown(self):
        for path in self.made:
            os.remove(path)

    def cache(self, *lines):
        path = cache_with(*lines)
        self.made.append(path)
        return path

    def test_msbuild_gets_one_project_at_a_time_and_a_bounded_compiler(self):
        # -m alone does not bound a JUCE build: /MP inside each project still takes every core.
        for generator in ("Visual Studio 17 2022", "Visual Studio 18 2026"):
            cache = self.cache(f"CMAKE_GENERATOR:INTERNAL={generator}")
            self.assertEqual(run.parallel_args(cache, 16), ["--", "-m:1", "-p:CL_MPCount=16"])

    def test_other_generators_take_the_count_directly(self):
        cache = self.cache("CMAKE_GENERATOR:INTERNAL=Ninja")
        self.assertEqual(run.parallel_args(cache, 8), ["--parallel", "8"])

    def test_a_missing_cache_still_gets_a_cap(self):
        self.assertEqual(run.parallel_args(os.path.join(tempfile.gettempdir(), "no-such-cache.txt"), 4),
                         ["--parallel", "4"])

    def test_the_count_defaults_to_build_jobs(self):
        cache = self.cache("CMAKE_GENERATOR:INTERNAL=Visual Studio 17 2022")
        with mock.patch.object(run, "build_jobs", return_value=3):
            self.assertEqual(run.parallel_args(cache), ["--", "-m:1", "-p:CL_MPCount=3"])

    def test_unit_tests_are_read_from_the_cache_whatever_the_entry_type(self):
        # -DBUILD_UNIT_TESTS=ON with no option() behind it is cached as UNINITIALIZED.
        self.assertTrue(run.unit_tests_are_on(self.cache("BUILD_UNIT_TESTS:UNINITIALIZED=ON")))
        self.assertTrue(run.unit_tests_are_on(self.cache("BUILD_UNIT_TESTS:BOOL=ON")))
        self.assertFalse(run.unit_tests_are_on(self.cache("BUILD_UNIT_TESTS:BOOL=OFF")))
        self.assertFalse(run.unit_tests_are_on(self.cache("LTO:BOOL=ON")))
        self.assertFalse(run.unit_tests_are_on(self.cache("BUILD_UNIT_TESTS_EXTRA:BOOL=ON")))


class BuildEnvironmentTests(unittest.TestCase):
    def test_the_compiler_is_bounded_for_nested_builds(self):
        env = {key: value for key, value in os.environ.items() if key.upper() != "CL_MPCOUNT"}
        with mock.patch.dict(os.environ, env, clear=True), mock.patch.object(run, "build_jobs", return_value=5):
            self.assertEqual(run.build_environment()["CL_MPCount"], "5")

    def test_a_count_already_set_is_left_alone(self):
        with mock.patch.dict(os.environ, {"CL_MPCount": "2"}), mock.patch.object(run, "build_jobs", return_value=5):
            counts = [value for key, value in run.build_environment().items() if key.upper() == "CL_MPCOUNT"]
        self.assertEqual(counts, ["2"])


class TreeTests(unittest.TestCase):
    """Tests against a build tree made of empty files: nothing here runs cmake or a compiler."""

    def setUp(self):
        self.tree = tempfile.TemporaryDirectory()
        self.build = self.tree.name
        self.cache = os.path.join(self.build, "CMakeCache.txt")

    def tearDown(self):
        self.tree.cleanup()

    def write_cache(self, *lines):
        with open(self.cache, "w", encoding="utf-8") as handle:
            handle.write("\n".join(lines) + "\n")

    def touch(self, *parts):
        path = os.path.join(self.build, *parts)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, "w").close()
        return path


class UnitTestsExeTests(TreeTests):
    def artefacts(self, *rest):
        return os.path.join(self.build, "Tests", "UnitTests_artefacts", *rest, "UnitTests.exe")

    def test_a_multi_config_tree_uses_the_configuration_asked_for(self):
        for generator in ("Visual Studio 18 2026", "Xcode", "Ninja Multi-Config"):
            self.write_cache(f"CMAKE_GENERATOR:INTERNAL={generator}", "CMAKE_BUILD_TYPE:STRING=Debug")
            self.assertEqual(run.unit_tests_exe(self.build, self.cache, "Release"), self.artefacts("Release"))

    def test_a_single_config_tree_uses_the_build_type_it_was_configured_with(self):
        # --config Release does not change what a Ninja tree configured as Debug builds.
        self.write_cache("CMAKE_GENERATOR:INTERNAL=Ninja", "CMAKE_BUILD_TYPE:STRING=Debug")
        self.assertEqual(run.unit_tests_exe(self.build, self.cache, "Release"), self.artefacts("Debug"))

    def test_a_single_config_tree_with_no_build_type_has_no_configuration_folder(self):
        self.write_cache("CMAKE_GENERATOR:INTERNAL=Ninja", "CMAKE_BUILD_TYPE:STRING=")
        self.assertEqual(run.unit_tests_exe(self.build, self.cache, "Release"), self.artefacts())

    def test_the_runner_finds_a_ninja_debug_binary_with_the_default_arguments(self):
        self.write_cache("CMAKE_GENERATOR:INTERNAL=Ninja", "CMAKE_BUILD_TYPE:STRING=Debug")
        exe = self.touch("Tests", "UnitTests_artefacts", "Debug", "UnitTests.exe")
        done = mock.Mock(returncode=0)
        with mock.patch.object(run.subprocess, "run", return_value=done) as ran, mock.patch("builtins.print"):
            self.assertEqual(run.run_unit_tests(self.build, self.cache, "Release"), 0)
        self.assertEqual(ran.call_args.args[0], [exe])

    def test_the_runner_passes_a_failure_on(self):
        self.write_cache("CMAKE_GENERATOR:INTERNAL=Visual Studio 18 2026")
        self.touch("Tests", "UnitTests_artefacts", "Release", "UnitTests.exe")
        with mock.patch.object(run.subprocess, "run", return_value=mock.Mock(returncode=3)), mock.patch("builtins.print"):
            self.assertEqual(run.run_unit_tests(self.build, self.cache, "Release"), 3)

    def test_the_runner_says_so_when_there_is_no_binary(self):
        self.write_cache("CMAKE_GENERATOR:INTERNAL=Visual Studio 18 2026")
        with mock.patch.object(run.subprocess, "run") as ran, mock.patch("builtins.print"):
            self.assertEqual(run.run_unit_tests(self.build, self.cache, "Release"), 1)
        ran.assert_not_called()


class ConfigureTests(TreeTests):
    VS = "CMAKE_GENERATOR:INTERNAL=Visual Studio 18 2026"

    def test_a_cold_tree_is_configured(self):
        self.assertTrue(run.needs_configure(self.build, self.cache, False))
        self.assertTrue(run.needs_configure(self.build, self.cache, True))

    def test_a_tree_with_lto_on_is_configured_again(self):
        self.touch("QuarryPlugin.sln")
        self.write_cache(self.VS, "LTO:BOOL=ON")
        self.assertTrue(run.needs_configure(self.build, self.cache, False))

    def test_a_generated_tree_is_left_alone_for_a_plain_build(self):
        self.touch("QuarryPlugin.sln")
        self.write_cache(self.VS, "LTO:BOOL=OFF")
        self.assertFalse(run.needs_configure(self.build, self.cache, False))

    def test_the_cache_alone_does_not_prove_the_tests_were_generated(self):
        self.touch("QuarryPlugin.sln")
        self.write_cache(self.VS, "LTO:BOOL=OFF", "BUILD_UNIT_TESTS:UNINITIALIZED=ON")
        self.assertTrue(run.needs_configure(self.build, self.cache, True))

    def test_a_stamp_without_the_cache_entry_is_not_enough_either(self):
        self.touch("QuarryPlugin.sln")
        self.touch(run.UNIT_TESTS_STAMP)
        self.write_cache(self.VS, "LTO:BOOL=OFF", "BUILD_UNIT_TESTS:BOOL=OFF")
        self.assertTrue(run.needs_configure(self.build, self.cache, True))

    def test_a_retry_after_a_failed_enable_tests_configure_configures_again(self):
        # An existing tree without the tests. The first configure saves BUILD_UNIT_TESTS=ON and
        # then fails, leaving the old solution in place. The retry must not go straight to the
        # build, and once a configure has finished the next run must not configure a third time.
        self.touch("QuarryPlugin.sln")
        self.write_cache(self.VS, "LTO:BOOL=OFF")

        def failing(args):
            self.assertIn("-DBUILD_UNIT_TESTS=ON", args)
            self.write_cache(self.VS, "LTO:BOOL=OFF", "BUILD_UNIT_TESTS:UNINITIALIZED=ON")
            return 1

        self.assertTrue(run.needs_configure(self.build, self.cache, True))
        with mock.patch.object(run, "run_cmake", side_effect=failing):
            self.assertFalse(run.configure_build_tree(self.build, True))

        self.assertTrue(run.unit_tests_are_on(self.cache))
        self.assertTrue(run.needs_configure(self.build, self.cache, True))

        with mock.patch.object(run, "run_cmake", return_value=0) as configured:
            self.assertTrue(run.configure_build_tree(self.build, True))
        self.assertEqual(configured.call_count, 1)
        self.assertFalse(run.needs_configure(self.build, self.cache, True))

    def test_a_failed_configure_takes_an_earlier_stamp_away(self):
        self.touch("QuarryPlugin.sln")
        self.touch(run.UNIT_TESTS_STAMP)
        self.write_cache(self.VS, "LTO:BOOL=OFF", "BUILD_UNIT_TESTS:BOOL=ON")
        self.assertFalse(run.needs_configure(self.build, self.cache, True))
        with mock.patch.object(run, "run_cmake", return_value=1):
            self.assertFalse(run.configure_build_tree(self.build, True))
        self.assertTrue(run.needs_configure(self.build, self.cache, True))

    def test_a_plain_configure_does_not_ask_for_the_tests_or_stamp_them(self):
        with mock.patch.object(run, "run_cmake", return_value=0) as configured:
            self.assertTrue(run.configure_build_tree(self.build, False))
        self.assertNotIn("-DBUILD_UNIT_TESTS=ON", configured.call_args.args[0])
        self.assertFalse(os.path.exists(os.path.join(self.build, run.UNIT_TESTS_STAMP)))


if __name__ == "__main__":
    unittest.main()
