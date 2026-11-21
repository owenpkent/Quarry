"""Contract tests for Installers/Windows/quarry.iss and compile_installer.

They read the Inno Setup script as text and mock subprocess: ISCC and signtool never run.
Run: py -m pytest tools/release -q
"""

import os
import re
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import release  # noqa: E402
import sign  # noqa: E402

with open(release.ISS, encoding="utf-8") as _f:
    ISS_TEXT = _f.read().replace("\r\n", "\n")


def setting(name):
    match = re.search(rf"^{name}=(.*)$", ISS_TEXT, re.MULTILINE)
    return match.group(1).strip() if match else None


class ScriptContractTests(unittest.TestCase):
    def test_app_id_is_stable(self):
        # Changing it orphans every existing install and breaks in-place upgrade.
        self.assertEqual(setting("AppId"), "Quarry")

    def test_per_machine(self):
        self.assertEqual(setting("PrivilegesRequired"), "admin")

    def test_is_uninstallable(self):
        self.assertEqual(setting("Uninstallable"), "yes")
        self.assertIsNotNone(setting("UninstallDisplayIcon"))
        self.assertIsNotNone(setting("AppPublisher"))
        self.assertIsNotNone(setting("AppVersion"))

    def test_output_name_matches_release_script(self):
        template = setting("OutputBaseFilename").replace("{#AppVersion}", "1.2.3")
        self.assertEqual(template + ".exe", release.installer_name("1.2.3"))

    def test_updater_switches_still_handled(self):
        self.assertIn("/RELAUNCH", ISS_TEXT)
        self.assertIn("{param:WAITPID|0}", ISS_TEXT)
        self.assertIn("--updated", ISS_TEXT)
        self.assertIn("--update-blocked", ISS_TEXT)
        self.assertIn("function PrepareToInstall", ISS_TEXT)

    def test_locked_checks_for_install_and_uninstall(self):
        self.assertIn("FileIsLocked", ISS_TEXT)
        self.assertIn("function InitializeUninstall", ISS_TEXT)

    def test_user_data_is_not_touched(self):
        for forbidden in ("{userappdata}", "{localappdata}", "{userdocs}", "{commonappdata}"):
            self.assertNotIn(forbidden, ISS_TEXT)
        self.assertNotIn("[UninstallDelete]", ISS_TEXT)

    def test_signing_block_is_gated(self):
        match = re.search(r"#ifdef SignUninstaller\n(.*?)#endif", ISS_TEXT, re.DOTALL)
        self.assertIsNotNone(match)
        self.assertIn("SignTool=quarrysign", match.group(1))
        self.assertIn("SignedUninstaller=yes", match.group(1))
        outside = ISS_TEXT.replace(match.group(0), "")
        self.assertNotIn("SignTool=", outside)
        self.assertNotIn("SignedUninstaller", outside)


class CompileInstallerTests(unittest.TestCase):
    def _compile(self, signtool_error=None, **kwargs):
        find = mock.Mock(return_value=r"C:\Kits\x64\signtool.exe", side_effect=signtool_error)
        with mock.patch.object(release, "find_iscc", return_value="ISCC.exe"), \
                mock.patch.object(release.sign, "find_signtool", find), \
                mock.patch.object(release.os, "makedirs"), \
                mock.patch.object(release.os.path, "exists", return_value=False), \
                mock.patch.object(release, "run_cmd") as run:
            release.compile_installer("1.2.3", **kwargs)
        return run.call_args[0][0]

    def test_unsigned_build_passes_no_sign_arguments(self):
        command = self._compile(sign_uninstaller=False)
        self.assertNotIn("/DSignUninstaller", command)
        self.assertFalse([a for a in command if a.startswith("/Squarrysign")])
        self.assertIn("/DAppVersion=1.2.3", command)

    def test_default_is_unsigned(self):
        self.assertNotIn("/DSignUninstaller", self._compile())

    def test_signed_build_passes_define_and_command(self):
        command = self._compile(sign_uninstaller=True)
        self.assertIn("/DSignUninstaller", command)
        arg = next(a for a in command if a.startswith("/Squarrysign="))
        self.assertTrue(arg.endswith(" $f"))
        self.assertIn(sign.CERTIFICATE_SHA1, arg)
        self.assertIn(sign.TIMESTAMP_SERVER, arg)
        self.assertIn("$qC:\\Kits\\x64\\signtool.exe$q", arg)
        self.assertNotIn('"', arg)

    def test_signing_failure_is_a_release_error(self):
        with self.assertRaises(release.ReleaseError):
            self._compile(signtool_error=sign.SigningError("no signtool"), sign_uninstaller=True)


if __name__ == "__main__":
    unittest.main()
