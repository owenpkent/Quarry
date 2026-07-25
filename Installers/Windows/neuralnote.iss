#ifndef ReleaseDir
#define ReleaseDir "cmake-build-release-visual-studio/NeuralNote_artefacts/Release"
#endif

[Setup]
AppName=NeuralNoteVideo
AppVersion=1.1.0
OutputBaseFilename=NeuralNoteVideoInstaller
DefaultDirName={pf}\NeuralNoteVideo
DefaultGroupName=NeuralNoteVideo
InfoBeforeFile=..\readme.txt
LicenseFile=..\license.txt
AppPublisher=Owen Kent
AppPublisherURL=https://github.com/owenpkent/NeuralNoteVideo
AppSupportURL=https://github.com/owenpkent/NeuralNoteVideo
AppUpdatesURL=https://github.com/owenpkent/NeuralNoteVideo
AlwaysShowComponentsList=yes
Compression=lzma
SolidCompression=yes
Uninstallable=no
DisableDirPage=yes
AppCopyright=Copyright (c) 2026 Owen Kent. Based on NeuralNote, Copyright (c) 2024 Damien Ronssin.

[Types]
Name: "custom"; Description: "Custom Installation"; Flags: iscustom

[Components]
Name: "mainapp"; Description: "NeuralNoteVideo Standalone"; Types: custom; Flags: disablenouninstallwarning
Name: "plugin"; Description: "NeuralNoteVideo VST3"; Types: custom; Flags: disablenouninstallwarning

[Files]
Source: "..\..\{#ReleaseDir}\Standalone\NeuralNoteVideo.exe"; DestDir: "{pf}\NeuralNoteVideo"; Components:mainapp; Flags: ignoreversion recursesubdirs;
Source: "..\..\{#ReleaseDir}\VST3\NeuralNoteVideo.vst3\*"; DestDir: "C:\Program Files\Common Files\VST3\NeuralNoteVideo.vst3"; Components:plugin; Flags: ignoreversion recursesubdirs;
