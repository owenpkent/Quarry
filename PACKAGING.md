# Package Quarry for macOS

- Build the app, VST3 and AU in Release mode (don't forget to turn on universal binary (`-DUniversalBinary=ON`) and
  LTO (`-DLTO=ON`).
- Install [Packages](http://s.sudre.free.fr/Software/Packages/about.html) if you don't have it already.
- Set up an Apple Developer certificate and load it into Keychain (for both the app and the installer).
- Run the `sign_and_package_quarry_macos.sh` script to sign the 3 artifacts and package them into a an
  installer (.pkg file).
    - Run the script with the path to the release directory containing the Standalone, VST3 and AU directory (usually
      `cmake-build-release/Quarry_artefacts/Release`).
      ```bash
      ./sign_and_package_quarry_macos.sh cmake-build-release/Quarry_artefacts/Release
      ``` 
    - Notarization credentials come from a `notarytool` keychain profile (named `Quarry` by default,
      override with the `NOTARY_PROFILE` environment variable). The first run creates it, prompting for your
      Apple ID and app-specific password; later runs reuse the stored profile and prompt for nothing.
    - The script signs with the `Developer ID Application` identity in your keychain and adds a secure
      timestamp, which notarization requires. If the keychain holds more than one such identity it stops
      rather than pick one for you; name the one you mean with the `SIGNING_ID` environment variable.
    - The installer will be located in `Installers/Mac/build`

# Package Quarry for Windows

On Windows, `tools/release/release.py` signs the binaries and the installer (see "Code signing" in `docs/AUTO_UPDATE.md`; the hardware token must be plugged in, or pass `--no-sign` for a dry run). To create the installer, the following steps are required:

- Build the app and VST3 in Release mode, configured with `-DLTO=OFF`. The prebuilt
  `onnxruntime.lib` is compiled with `/GL` by one specific MSVC version, so with link-time
  optimisation on, linking fails with `C1047` unless your compiler matches it. `run.py` passes
  that flag for you; a build tree you configured by hand needs it explicitly, because `LTO`
  defaults to `ON`.
- Install [Inno Setup](https://jrsoftware.org/isinfo.php) if you don't have it already.
- Build the installer.
    - In command prompt, from the trunk of the repository, run the following command:
      ```commandline
      "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" Installers\Windows\quarry.iss /DAppVersion=1.1.0 /DReleaseDir="cmake-build-release/Quarry_artefacts/Release"
      ```
      `DAppVersion` is required and has no default (the script stops with an error without it), so the
      installer's version cannot drift from `project(QuarryPlugin VERSION ...)` in `CMakeLists.txt`.
      For a real release use `tools/release/release.py` below instead of running ISCC by hand.
      `DReleaseDir` should indicate the path to the release directory containing the Standalone and VST3
      directories (there is no AU on Windows).

The installer asks for administrator rights (`PrivilegesRequired=admin`) because it writes to machine-wide
folders: the standalone app goes to `{commonpf}\Quarry` and the VST3 to `{commoncf}\VST3`. Those are Inno Setup
constants that resolve to the real Program Files and Common Files folders, so the installer no longer
assumes Windows lives on `C:`.

The installer is uninstallable. It has a fixed `AppId=Quarry` (never change it: it is what ties an
upgrade and the Add/Remove Programs entry to the existing install). Uninstalling removes the app folder
and, if it was installed, the VST3 bundle in `{commoncf}\VST3`. It does not touch settings, samples or
models under AppData or Documents. If Quarry or a DAW has a file open, the uninstaller says so and stops (a file only counts if this installation put it down).
`release.py` signs the uninstaller by passing ISCC `/DSignUninstaller` and a `/Squarrysign=...` command
(omitted with `--no-sign`); see "Code signing" in `docs/AUTO_UPDATE.md`.

The installer will be located in `Installers/Windows/Output`.

The licence page shows `Installers/license.txt`: a plain-language notice (as is, no warranty, no
liability, what you record is your responsibility, privacy, updates) followed by the Apache-2.0
text. The notice is a summary and says so; the licence is what applies. Keep the two in step with
`LICENSE` and with the "No warranty" section of the README, and do not reword the notice without
the owner: it is the nearest thing Quarry has to terms of use. A silent install, which is what the
updater runs, shows no pages, so the notice says that an update comes on the same terms. The
installer also copies `LICENSE` and `NOTICE` to `Program Files\Quarry` as `.txt` files. The
Windows installer shows no "Information" page (`InfoBeforeFile`): `Installers/readme.txt` repeated
the licence page, so only the macOS package uses it now.
`tools/release/test_installer.py` checks all of this.

## Dependency scans

Three things watch the dependencies. None of them is part of the release script.

- **OSV scan** (`ci.yml` jobs `osv-scan` and `osv-scan-main`, and `.github/workflows/osv-nightly.yml`)
  reads `tools/sidecar/requirements.txt` and `prototypes/synthesia_detector/requirements.txt` and
  fails on any known advisory (`fail-on-vuln: true`). On a pull request it fails only on
  advisories the PR introduces; master is scanned in full on every push and nightly. The git
  submodules and the pinned onnxruntime are not scanned, because the release SBOM is only written
  by a full build.
- **Dependabot** (`.github/dependabot.yml`) opens weekly PRs for GitHub Actions, both Python
  requirements directories and the RTNeural submodule. JUCE is ignored and bumped by hand: it is
  pinned to a security tag and JUCE 9 is a separate migration.
- **Socket** (`socket.yml`) checks dependency changes on PRs for supply-chain alerts and ignores
  `ThirdParty/`.

To quarantine an advisory that has no fix yet, add an `osv-scanner.toml` beside the requirements
file it applies to (the scanner reads it only from that file's directory), with a reason and a
short expiry:

```toml
[[IgnoredVulns]]
id = "GHSA-xxxx-xxxx-xxxx"
ignoreUntil = 2026-12-31
reason = "No fixed release yet; not reachable from the sidecar."
```

Use weeks, not months. When the date passes the scan fails again, which is the reminder to check
for a fix. Never set `fail-on-vuln` to false.

A failing scan on master blocks a release: fix or pin the dependency, or add a time-boxed ignore
with a reason, before running `release.py build`. A release should not ship with an open,
unexplained advisory.

## Windows release checklist

A release is manual and local. `tools/release/release.py` builds, checks and publishes it; the
version comes from `project(QuarryPlugin VERSION X.Y.Z)` in `CMakeLists.txt` and nowhere else. The
file names, the tag and the installer flags are a contract with the in-app updater (see
`docs/AUTO_UPDATE.md`). Needs Inno Setup 6 (`ISCC.exe`, or set `QUARRY_ISCC`), the GitHub CLI
(`gh`) and the usual build tools.

The order below follows the playbook alpha-osk releases by, and the same rule heads it: do not
skip steps.

1. Bump the version in `CMakeLists.txt` (`project(QuarryPlugin VERSION X.Y.Z)`). Always three
   parts. The tags `v1.0.0` and `v1.1.0` are NeuralNote's and came with the fork, so Quarry's
   own releases start at 1.2.0.
2. In `CHANGELOG.md`, rename `[Unreleased]` to `## [X.Y.Z] - YYYY-MM-DD` and add a fresh empty
   `[Unreleased]` above it. The section becomes the release notes. If the release is published on
   a later day than the heading says, correct the date first.
3. Land it through a pull request: branch `chore/release-X.Y.Z` from `origin/master`, commit
   `chore(release): X.Y.Z`, open the PR, and merge it once CI is green, the OSV scan included.
   Then bring the main checkout to the merge: `git switch master`, `git pull --ff-only`.
4. Build everything, **from the main checkout after the merge**, never from the release branch's
   worktree: that folder goes away when the branch does, and the installer with it. Use a normal
   shell, not an elevated one (the token is invisible to elevated processes), with the hardware
   token plugged in:
   ```commandline
   py tools/release/release.py build
   ```
   The token's own software asks for the PIN, which only the owner enters. If every signing
   attempt times out at 60 seconds, the token is waiting for that PIN. A first pass with
   `--no-sign` needs no token and finds everything except signing problems; its output cannot be
   published.
   This checks the CHANGELOG section and that tag `vX.Y.Z` is free locally and on origin, configures
   `build-release/` (Release, Standalone + VST3 + UnitTests, `-DLTO=OFF` for the reason given
   above; `--lto` turns it on), runs `UnitTests`, compiles the installer, and writes to `release/`:
   `QuarrySetup-X.Y.Z.exe`, `QuarrySetup-X.Y.Z.exe.sha256`, `Quarry-X.Y.Z-sbom.cyclonedx.json`
   (the git submodules plus the prebuilt onnxruntime with its pinned SHA-256) and
   `release-notes-X.Y.Z.md`. It then runs the gates: `Quarry.exe` and the VST3 report `X.Y.Z`, the
   installer reports `X.Y.Z.0`, the tree is clean. `--verify-only` runs just the gates,
   `--skip-build` reuses `build-release/`, `--no-installer` skips ISCC. There is no lockfile: the
   installer ships only `Quarry.exe`, the VST3 and two text files (`LICENSE.txt` and
   `NOTICE.txt`), nothing written in Python.
5. Smoke test the installer from `release/`:
   - Interactively: the same pages and components as always, and the app and VST3 land in
     `Program Files\Quarry` and `Common Files\VST3`. The licence page opens on the plain-language
     notice (as is, no warranty, no liability, what you record is your responsibility), and Next
     stays disabled until "I accept the agreement" is chosen. `LICENSE.txt` and `NOTICE.txt` are
     in `Program Files\Quarry` afterwards, with either component or both.
   - Silently, with the flags the updater uses, from an unelevated prompt (it self-elevates):
     ```commandline
     release\QuarrySetup-X.Y.Z.exe /VERYSILENT /SUPPRESSMSGBOXES /NORESTART /NOCLOSEAPPLICATIONS /COMPONENTS="mainapp,plugin" /WAITPID=<pid of a running Quarry> /RELAUNCH
     ```
     It should wait for that process to exit, install, and start `Quarry.exe --updated`. With a DAW
     holding the VST3 open it should install nothing and start `Quarry.exe --update-blocked`.
6. Tag and push:
   ```commandline
   git tag vX.Y.Z
   git push origin master vX.Y.Z
   ```
7. Publish:
   ```commandline
   py tools/release/release.py publish          # prints the gh release create command
   py tools/release/release.py publish --yes    # runs it
   ```
   It refuses unless the gates pass and the tag is on origin. The release is `vX.Y.Z` on
   `owenpkent/Quarry`, never a draft or prerelease, with the installer, the `.sha256` and the SBOM
   attached.

Windows builds are signed with the OK Studio Inc. EV certificate, and `publish` refuses an
installer that is not (override with `--allow-unsigned`). The published `.sha256` file is the hash
of the signed installer (`sha256sum -c QuarrySetup-X.Y.Z.exe.sha256`).
