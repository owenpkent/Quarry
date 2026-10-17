"""Tests for the pure logic in release.py. Run: py -m unittest discover tools/release"""

import json
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import release  # noqa: E402

CHANGELOG = """# Changelog

## [Unreleased]

### Added

- next thing

## [1.2.0] - 2026-10-16

### Fixed

- fix: a thing

## [1.1.0]

- older

[Unreleased]: https://example.invalid/compare
"""


class VersionTests(unittest.TestCase):
    def test_parse(self):
        text = "cmake_minimum_required(VERSION 3.16)\nproject(QuarryPlugin VERSION 1.1.0)\n"
        self.assertEqual(release.parse_cmake_version(text), "1.1.0")

    def test_missing(self):
        with self.assertRaises(release.ReleaseError):
            release.parse_cmake_version("project(X)")

    def test_not_three_parts(self):
        with self.assertRaises(release.ReleaseError):
            release.parse_cmake_version("project(QuarryPlugin VERSION 1.1)")

    def test_repo_version_parses(self):
        self.assertRegex(release.read_version(), r"^\d+\.\d+\.\d+$")


class ChangelogTests(unittest.TestCase):
    def test_dated_heading(self):
        self.assertEqual(release.changelog_section(CHANGELOG, "1.2.0"), "### Fixed\n\n- fix: a thing")

    def test_last_section_stops_at_link_block(self):
        self.assertEqual(release.changelog_section(CHANGELOG, "1.1.0"), "- older")

    def test_missing(self):
        self.assertIsNone(release.changelog_section(CHANGELOG, "9.9.9"))

    def test_no_prefix_match(self):
        self.assertIsNone(release.changelog_section(CHANGELOG, "1.2"))

    def test_crlf(self):
        self.assertEqual(release.changelog_section(CHANGELOG.replace("\n", "\r\n"), "1.1.0"), "- older")

    def test_empty_section_is_none(self):
        self.assertIsNone(release.changelog_section("## [1.0.0]\n\n## [0.9.0]\n- x\n", "1.0.0"))


class NamingTests(unittest.TestCase):
    def test_names(self):
        self.assertEqual(release.installer_name("1.2.3"), "QuarrySetup-1.2.3.exe")
        self.assertEqual(release.sbom_name("1.2.3"), "Quarry-1.2.3-sbom.cyclonedx.json")

    def test_sha_line(self):
        self.assertEqual(release.sha256_line("AB" * 32, "QuarrySetup-1.2.3.exe"),
                         "ab" * 32 + "  QuarrySetup-1.2.3.exe\n")

    def test_sha_line_rejects_bad_digest(self):
        with self.assertRaises(ValueError):
            release.sha256_line("abc", "x")

    def test_version_tuple(self):
        self.assertEqual(release.version_tuple_string((1, 1, 0, 0)), "1.1.0.0")


class SbomTests(unittest.TestCase):
    def test_purl(self):
        self.assertEqual(release.github_purl("https://github.com/juce-framework/JUCE.git", "a" * 40),
                         "pkg:github/juce-framework/juce@" + "a" * 40)
        self.assertIsNone(release.github_purl("https://example.org/x.git", "a" * 40))

    def test_parse_status(self):
        text = (" " + "1" * 40 + " ThirdParty/JUCE (8.0.6)\n"
                "+" + "2" * 40 + " ThirdParty/RTNeural (2ca066e)\n")
        self.assertEqual(release.parse_submodule_status(text),
                         [("ThirdParty/JUCE", "1" * 40, "8.0.6"),
                          ("ThirdParty/RTNeural", "2" * 40, "2ca066e")])

    def test_sbom(self):
        sbom = release.build_sbom(
            "1.2.3",
            [{"name": "JUCE", "path": "ThirdParty/JUCE", "commit": "a" * 40,
              "url": "https://github.com/juce-framework/JUCE.git", "describe": "8.0.6"}],
            {"version": "v1", "url": "https://example.invalid/o.tar.gz", "sha256": "b" * 64},
            "2026-10-16T00:00:00Z", "00000000-0000-0000-0000-000000000000")
        json.dumps(sbom)
        self.assertEqual(sbom["bomFormat"], "CycloneDX")
        self.assertEqual(sbom["specVersion"], "1.6")
        self.assertEqual(sbom["metadata"]["component"]["version"], "1.2.3")
        self.assertEqual([c["name"] for c in sbom["components"]], ["JUCE", "onnxruntime"])
        self.assertEqual(sbom["components"][0]["version"], "a" * 40)
        self.assertTrue(sbom["components"][0]["purl"].startswith("pkg:github/"))
        self.assertEqual(sbom["components"][1]["hashes"][0]["content"], "b" * 64)
        refs = [c["bom-ref"] for c in sbom["components"]]
        self.assertEqual(len(refs), len(set(refs)))


class PublishTests(unittest.TestCase):
    def test_gh_command(self):
        command = release.gh_create_command("1.2.3", ["a", "b", "c"])
        self.assertEqual(command[:4], ["gh", "release", "create", "v1.2.3"])
        self.assertIn("--verify-tag", command)
        self.assertEqual(command[command.index("--title") + 1], "v1.2.3")
        self.assertEqual(command[command.index("--repo") + 1], "owenpkent/Quarry")
        self.assertNotIn("--draft", command)
        self.assertNotIn("--prerelease", command)


if __name__ == "__main__":
    unittest.main()
