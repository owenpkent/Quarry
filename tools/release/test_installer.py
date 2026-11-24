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

    def test_ownership_is_recorded_only_with_the_owning_component(self):
        for value, component in (("InstalledVst3", "plugin"), ("InstalledStandalone", "mainapp")):
            entries = [line for line in ISS_TEXT.splitlines() if f'ValueName: "{value}"' in line]
            self.assertEqual(len(entries), 1, value)
            self.assertIn("Root: HKLM;", entries[0])
            self.assertIn(f"Components: {component};", entries[0])
            self.assertIn("uninsdeletevalue", entries[0])

    def test_uninstall_ignores_locked_files_it_does_not_own(self):
        # Both components are optional. A standalone-only install must not be blocked by a DAW
        # holding a Quarry.vst3 this installation never put down, and a plugin-only install must
        # not be blocked by a running Quarry.exe it never put down: every lock check in the
        # uninstaller is gated on the ownership record of the file it looks at.
        body = ISS_TEXT[ISS_TEXT.index("function InitializeUninstall"):]
        checks = re.findall(r"(\w+ and\s+)?FileIsLocked\(ExpandConstant\('([^']*)'\)\)", body)
        self.assertEqual(len(checks), 2)
        for gate, path in checks:
            owner = "InstalledVst3" if path.endswith(".vst3") else "InstalledStandalone"
            self.assertTrue(gate.startswith(owner + " and"), path)
        for value in ("InstalledVst3", "InstalledStandalone"):
            self.assertIn(f"RegValueExists(HKLM, 'Software\\OK Studio\\Quarry', '{value}')", ISS_TEXT)

    def test_licence_page_is_shown_and_says_no_warranty(self):
        # The licence page is where the user accepts that Quarry comes as is. Inno keeps Next
        # disabled on it until "I accept the agreement" is chosen, but only if LicenseFile is set.
        licence_setting = setting("LicenseFile") or ""
        self.assertTrue(licence_setting, "LicenseFile is not set")
        licence_path = os.path.normpath(os.path.join(os.path.dirname(release.ISS), licence_setting))
        with open(licence_path, encoding="utf-8") as handle:
            licence = handle.read()
        # The plain-language notice comes before the licence text, not after 150 lines of it.
        notice, _, rest = licence.partition("Apache License")
        for phrase in ('"as is"', "No warranty", "No liability", "at your own risk",
                       "What you record is your responsibility"):
            self.assertIn(phrase, notice, phrase)
        # The privacy point must not promise "GitHub only": verifying an update's signature has
        # Windows check revocation, which can reach the certificate authority.
        self.assertIn("certificate authority", notice)
        self.assertNotIn("only network requests", notice)
        # And the licence itself is still there in full, with the two sections the notice cites.
        for phrase in ("7. Disclaimer of Warranty", "8. Limitation of Liability"):
            self.assertIn(phrase, rest, phrase)

    def test_no_information_page_repeats_the_licence_page(self):
        # The readme used to be shown as an "Information" page before the licence. It said again
        # what the licence page says (privacy, the NeuralNote credit, the source link), and a
        # second wall of text makes the page that matters less likely to be read.
        self.assertIsNone(setting("InfoBeforeFile"))
        self.assertIsNone(setting("InfoAfterFile"))

    def test_standalone_gets_start_menu_and_desktop_shortcuts(self):
        # 1.2.0 shipped with no [Icons] section, so Quarry never appeared in the Start menu.
        icons = ISS_TEXT[ISS_TEXT.index("[Icons]"):ISS_TEXT.index("[Registry]")]
        for name, task in (('"{group}\\Quarry"', "startmenu"),
                           ('"{commondesktop}\\Quarry"', "desktopicon")):
            entries = [line for line in icons.splitlines() if line.startswith(f"Name: {name};")]
            self.assertEqual(len(entries), 1, name)
            self.assertIn('Filename: "{app}\\Quarry.exe";', entries[0])
            self.assertIn("Components: mainapp;", entries[0])
            self.assertIn(f"Tasks: {task}", entries[0])
            task_lines = [line for line in ISS_TEXT.splitlines()
                          if line.startswith(f'Name: "{task}";')]
            self.assertEqual(len(task_lines), 1, task)
            self.assertNotIn("unchecked", task_lines[0])
        self.assertEqual(setting("DisableProgramGroupPage"), "yes")

    def test_licence_and_notices_are_installed_with_either_component(self):
        files = ISS_TEXT[ISS_TEXT.index("[Files]"):ISS_TEXT.index("[Registry]")]
        for source, dest in (("LICENSE", "LICENSE.txt"), ("NOTICE", "NOTICE.txt")):
            entries = [line for line in files.splitlines()
                       if line.startswith("Source:") and f'DestName: "{dest}"' in line]
            self.assertEqual(len(entries), 1, dest)
            self.assertIn(f'Source: "..\\..\\{source}";', entries[0])
            self.assertIn('DestDir: "{app}";', entries[0])
            self.assertNotIn("Components:", entries[0])
            repo_root = os.path.normpath(os.path.join(os.path.dirname(release.ISS), "..", ".."))
            self.assertTrue(os.path.isfile(os.path.join(repo_root, source)), source)

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
