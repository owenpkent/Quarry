"""Tests for the pure logic in release.py. Run: py -m unittest discover tools/release"""

import json
import os
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import release  # noqa: E402
import sign  # noqa: E402

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


def _done(code=0, out=""):
    return subprocess.CompletedProcess([], code, stdout=out, stderr="")


class SignCommandTests(unittest.TestCase):
    def test_sign_command(self):
        cmd = sign.sign_command("signtool.exe", "Quarry.exe")
        self.assertEqual(cmd[:2], ["signtool.exe", "sign"])
        self.assertEqual(cmd[cmd.index("/sha1") + 1], "fc22b5221318f3f3f6b3eb2d969d7f99091557bf")
        self.assertEqual(cmd[cmd.index("/fd") + 1], "sha256")
        self.assertEqual(cmd[cmd.index("/tr") + 1], "http://timestamp.digicert.com")
        self.assertEqual(cmd[cmd.index("/td") + 1], "sha256")
        self.assertEqual(cmd[-1], "Quarry.exe")

    def test_verify_command(self):
        self.assertEqual(sign.verify_command("s", "f")[1:3], ["verify", "/pa"])

    def test_retries_then_succeeds(self):
        with mock.patch.object(sign.subprocess, "run", side_effect=[_done(1), _done(1), _done(0)]) as run, \
                mock.patch.object(sign.time, "sleep") as sleep:
            sign.sign_file("Quarry.exe", "signtool.exe")
        self.assertEqual(run.call_count, 3)
        self.assertEqual([c.args[0] for c in sleep.call_args_list], [2.0, 4.0])

    def test_timeout_is_retried(self):
        timeout = subprocess.TimeoutExpired("signtool", 60)
        with mock.patch.object(sign.subprocess, "run", side_effect=[timeout, _done(0)]) as run, \
                mock.patch.object(sign.time, "sleep"):
            sign.sign_file("Quarry.exe", "signtool.exe")
        self.assertEqual(run.call_count, 2)

    def test_gives_up_after_max_retries(self):
        with mock.patch.object(sign.subprocess, "run", return_value=_done(1)) as run, \
                mock.patch.object(sign.time, "sleep"):
            with self.assertRaises(sign.SigningError):
                sign.sign_file("Quarry.exe", "signtool.exe")
        self.assertEqual(run.call_count, sign.MAX_RETRIES)

    def test_signature_problem_wrong_signer(self):
        with mock.patch.object(sign.os.path, "isfile", return_value=True), \
                mock.patch.object(sign.subprocess, "run", return_value=_done(0, "Valid|" + "A" * 40)):
            self.assertIn("pinned", sign.signature_problem("x.exe", "signtool.exe"))

    def test_signature_problem_not_signed(self):
        with mock.patch.object(sign.os.path, "isfile", return_value=True), \
                mock.patch.object(sign.subprocess, "run", return_value=_done(0, "NotSigned|")):
            self.assertIn("NotSigned", sign.signature_problem("x.exe", "signtool.exe"))

    def test_signature_problem_passes_the_path_as_data(self):
        with mock.patch.object(sign.os.path, "isfile", return_value=True), \
                mock.patch.object(sign.subprocess, "run", return_value=_done(0, "NotSigned|")) as run:
            sign.signature_problem("x.exe", "signtool.exe")
        command = run.call_args.args[0]
        self.assertEqual(command[-2], "-Command")  # the script is the last argument: no path after it
        self.assertEqual(run.call_args.kwargs["env"]["QUARRY_SIGNATURE_PATH"], os.path.abspath("x.exe"))

    def test_signature_problem_ok(self):
        out = "Valid|" + sign.CERTIFICATE_SHA1.upper()
        with mock.patch.object(sign.os.path, "isfile", return_value=True), \
                mock.patch.object(sign.subprocess, "run", side_effect=[_done(0, out), _done(0)]):
            self.assertIsNone(sign.signature_problem("x.exe", "signtool.exe"))



@unittest.skipUnless(sys.platform == "win32", "asks the real Windows PowerShell")
class RealSignatureCheckTests(unittest.TestCase):
    """Not mocked: the mocked tests above cannot tell whether the path reaches PowerShell."""

    def test_a_microsoft_binary_is_valid_but_the_wrong_signer(self):
        powershell = os.path.join(os.environ["SystemRoot"], "System32", "WindowsPowerShell", "v1.0",
                                  "powershell.exe")
        problem = sign.signature_problem(powershell, "signtool.exe")
        # Read and judged Valid, then refused for its signer. "status is unknown" here would mean
        # the path never arrived.
        self.assertIsNotNone(problem)
        self.assertIn("not the pinned", problem)

    def test_an_unsigned_script_is_not_signed(self):
        with tempfile.TemporaryDirectory() as folder:
            path = os.path.join(folder, "with space", "unsigned.ps1")
            os.makedirs(os.path.dirname(path))
            with open(path, "w", encoding="ascii") as handle:
                handle.write("Write-Output 1\n")
            self.assertIn("NotSigned", sign.signature_problem(path, "signtool.exe"))


class BuildOrderTests(unittest.TestCase):
    def _run_build(self, **flags):
        calls = []

        def rec(name, result=None):
            return mock.Mock(side_effect=lambda *a, **k: calls.append(name) or result)

        args = types.SimpleNamespace(verify_only=False, allow_dirty=True, skip_build=False,
                                     no_installer=False, no_sign=False, lto=False)
        for key, value in flags.items():
            setattr(args, key, value)
        with mock.patch.object(release, "read_version", return_value="1.2.3"), \
                mock.patch.object(release, "check_tree_clean"), \
                mock.patch.object(release, "check_changelog", return_value="notes"), \
                mock.patch.object(release, "check_tag_is_free"), \
                mock.patch.object(release, "build_binaries", rec("build")), \
                mock.patch.object(release, "check_binary_versions"), \
                mock.patch.object(release, "compile_installer", rec("installer")), \
                mock.patch.object(release, "write_artifacts", rec("hash", [])), \
                mock.patch.object(release, "run_gates"), \
                mock.patch.object(release.sign, "find_signtool", return_value="signtool.exe"), \
                mock.patch.object(release.sign, "sign_file",
                                  side_effect=lambda p, t=None: calls.append("sign:" + os.path.basename(p))), \
                mock.patch.object(release.sign, "verify_file", return_value=True), \
                mock.patch.object(release.sign, "signature_problem", return_value=None):
            release.cmd_build(args)
        return calls

    def test_sign_before_hash(self):
        calls = self._run_build()
        self.assertEqual(calls, ["build", "sign:Quarry.exe", "sign:Quarry.vst3", "installer",
                                 "sign:QuarrySetup-1.2.3.exe", "hash"])

    def test_no_sign(self):
        calls = self._run_build(no_sign=True)
        self.assertEqual(calls, ["build", "installer", "hash"])

    def test_parser_flags(self):
        parser = release.make_parser()
        self.assertFalse(parser.parse_args(["build"]).no_sign)
        self.assertTrue(parser.parse_args(["build", "--no-sign"]).no_sign)
        self.assertFalse(parser.parse_args(["publish"]).allow_unsigned)
        self.assertTrue(parser.parse_args(["publish", "--allow-unsigned"]).allow_unsigned)


class PublishSigningTests(unittest.TestCase):
    def _publish(self, problem, allow_unsigned):
        args = types.SimpleNamespace(allow_dirty=True, yes=False, allow_unsigned=allow_unsigned)
        check = mock.Mock(return_value=problem)
        with mock.patch.object(release, "read_version", return_value="1.2.3"), \
                mock.patch.object(release, "check_changelog"), \
                mock.patch.object(release, "run_gates"), \
                mock.patch.object(release.os.path, "isfile", return_value=True), \
                mock.patch.object(release, "tag_on_origin", return_value="a" * 40), \
                mock.patch.object(release, "tag_exists_locally", return_value=False), \
                mock.patch.object(release.sign, "signature_problem", check):
            result = release.cmd_publish(args)
        return result, check

    def test_refuses_unsigned(self):
        with self.assertRaises(release.ReleaseError) as caught:
            self._publish("Authenticode status is NotSigned, not Valid", False)
        self.assertIn("--allow-unsigned", str(caught.exception))

    def test_allow_unsigned_skips_check(self):
        result, check = self._publish("anything", True)
        self.assertEqual(result, 0)
        check.assert_not_called()

    def test_signed_passes(self):
        result, check = self._publish(None, False)
        self.assertEqual(result, 0)
        check.assert_called_once()


if __name__ == "__main__":
    unittest.main()
