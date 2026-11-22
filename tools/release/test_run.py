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


if __name__ == "__main__":
    unittest.main()
