#ifndef ReleaseDir
#define ReleaseDir "cmake-build-release-visual-studio/Quarry_artefacts/Release"
#endif

#define AppVersion "1.1.0"

[Setup]
AppName=Quarry
AppVersion={#AppVersion}
OutputBaseFilename=QuarrySetup-{#AppVersion}
DefaultDirName={pf}\Quarry
DefaultGroupName=Quarry
InfoBeforeFile=..\readme.txt
LicenseFile=..\license.txt
AppPublisher=OK Studio
AppPublisherURL=https://github.com/owenpkent/Quarry
AppSupportURL=https://github.com/owenpkent/Quarry
AppUpdatesURL=https://github.com/owenpkent/Quarry
AlwaysShowComponentsList=yes
Compression=lzma
SolidCompression=yes
Uninstallable=no
DisableDirPage=yes
AppCopyright=Copyright (c) 2026 OK Studio. Based on NeuralNote, Copyright (c) 2024 Damien Ronssin.

[Types]
Name: "custom"; Description: "Custom Installation"; Flags: iscustom

[Components]
Name: "mainapp"; Description: "Quarry Standalone"; Types: custom; Flags: disablenouninstallwarning
Name: "plugin"; Description: "Quarry VST3"; Types: custom; Flags: disablenouninstallwarning

[Files]
Source: "..\..\{#ReleaseDir}\Standalone\Quarry.exe"; DestDir: "{pf}\Quarry"; Components:mainapp; Flags: ignoreversion recursesubdirs;
Source: "..\..\{#ReleaseDir}\VST3\Quarry.vst3\*"; DestDir: "C:\Program Files\Common Files\VST3\Quarry.vst3"; Components:plugin; Flags: ignoreversion recursesubdirs;
