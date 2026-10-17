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

On Windows, Quarry is not code signed for now. To create the installer, the following steps are required:

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

The installer will be located in `Installers/Windows/Output`.

## Windows release checklist

A release is manual and local. `tools/release/release.py` builds, checks and publishes it; the
version comes from `project(QuarryPlugin VERSION X.Y.Z)` in `CMakeLists.txt` and nowhere else. The
file names, the tag and the installer flags are a contract with the in-app updater (see
`docs/AUTO_UPDATE.md`). Needs Inno Setup 6 (`ISCC.exe`, or set `QUARRY_ISCC`), the GitHub CLI
(`gh`) and the usual build tools.

1. Bump the version in `CMakeLists.txt` (`project(QuarryPlugin VERSION X.Y.Z)`).
2. In `CHANGELOG.md`, rename `[Unreleased]` to `## [X.Y.Z] - YYYY-MM-DD` and add a fresh empty
   `[Unreleased]` above it. The section becomes the release notes.
3. Commit: `chore: bump version to X.Y.Z`. The working tree must be clean (`--allow-dirty` overrides).
4. Build everything:
   ```commandline
   py tools/release/release.py build
   ```
   This checks the CHANGELOG section and that tag `vX.Y.Z` is free locally and on origin, configures
   `build-release/` (Release, Standalone + VST3 + UnitTests, `-DLTO=OFF` for the reason given
   above; `--lto` turns it on), runs `UnitTests`, compiles the installer, and writes to `release/`:
   `QuarrySetup-X.Y.Z.exe`, `QuarrySetup-X.Y.Z.exe.sha256`, `Quarry-X.Y.Z-sbom.cyclonedx.json`
   (the git submodules plus the prebuilt onnxruntime with its pinned SHA-256) and
   `release-notes-X.Y.Z.md`. It then runs the gates: `Quarry.exe` and the VST3 report `X.Y.Z`, the
   installer reports `X.Y.Z.0`, the tree is clean. `--verify-only` runs just the gates,
   `--skip-build` reuses `build-release/`, `--no-installer` skips ISCC. There is no lockfile: the
   installer ships only `Quarry.exe` and the VST3, nothing written in Python.
5. Smoke test the installer from `release/`:
   - Interactively: the same pages and components as always, and the app and VST3 land in
     `Program Files\Quarry` and `Common Files\VST3`.
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

Windows builds are not code signed yet, so SmartScreen will warn on first run. Until signing is
added, the published `.sha256` file is the only integrity check (`sha256sum -c
QuarrySetup-X.Y.Z.exe.sha256`). Code signing is the follow-up.
