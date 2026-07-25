# Package NeuralNoteVideo for macOS

- Build the app, VST3 and AU in Release mode (don't forget to turn on universal binary (`-DUniversalBinary=ON`) and
  LTO (`-DLTO=ON`).
- Install [Packages](http://s.sudre.free.fr/Software/Packages/about.html) if you don't have it already.
- Set up an Apple Developer certificate and load it into Keychain (for both the app and the installer).
- Run the `sign_and_package_neuralnote_macos.sh` script to sign the 3 artifacts and package them into a an
  installer (.pkg file).
    - Run the script with the path to the release directory containing the Standalone, VST3 and AU directory (usually
      `cmake-build-release/NeuralNote_artefacts/Release`).
      ```bash
      ./sign_and_package_neuralnote_macos.sh cmake-build-release/NeuralNote_artefacts/Release
      ``` 
    - Notarization credentials come from a `notarytool` keychain profile (named `NeuralNoteVideo` by default,
      override with the `NOTARY_PROFILE` environment variable). The first run creates it, prompting for your
      Apple ID and app-specific password; later runs reuse the stored profile and prompt for nothing.
    - The installer will be located in `Installers/Mac/build`

# Package NeuralNoteVideo for Windows

On Windows, NeuralNoteVideo is not code signed for now. To create the installer, the following steps are required:

- Build the app and VST3 in Release mode.
- Install [Inno Setup](https://jrsoftware.org/isinfo.php) if you don't have it already.
- Build the installer.
    - In command prompt, from the trunk of the repository, run the following command:
      ```commandline
      "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" Installers\Windows\neuralnote.iss /DReleaseDir="cmake-build-release/NeuralNote_artefacts/Release"
      ```
      `DReleaseDir` should indicate the path to the release directory containing the Standalone and VST3
      directories (there is no AU on Windows).

The installer will be located in `Installers/Windows/Output`.