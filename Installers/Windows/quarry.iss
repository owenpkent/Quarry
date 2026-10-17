#ifndef ReleaseDir
#define ReleaseDir "cmake-build-release-visual-studio/Quarry_artefacts/Release"
#endif

; The version has one source, project(QuarryPlugin VERSION ...) in CMakeLists.txt, and
; tools/release/release.py passes it in. There is deliberately no default here.
#ifndef AppVersion
#error AppVersion is not defined. Compile with /DAppVersion=X.Y.Z (tools/release/release.py does).
#endif

[Setup]
AppName=Quarry
PrivilegesRequired=admin
; Quarry is x64 only. Without 64-bit install mode {commonpf} and {commoncf} resolve to the
; (x86) folders, and a 64-bit host does not scan Common Files (x86)\VST3.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
AppVersion={#AppVersion}
VersionInfoVersion={#AppVersion}.0
VersionInfoProductVersion={#AppVersion}.0
VersionInfoProductName=Quarry
VersionInfoCompany=OK Studio
VersionInfoDescription=Quarry Setup
OutputBaseFilename=QuarrySetup-{#AppVersion}
DefaultDirName={commonpf}\Quarry
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
Source: "..\..\{#ReleaseDir}\Standalone\Quarry.exe"; DestDir: "{commonpf}\Quarry"; Components:mainapp; Flags: ignoreversion recursesubdirs;
Source: "..\..\{#ReleaseDir}\VST3\Quarry.vst3\*"; DestDir: "{commoncf}\VST3\Quarry.vst3"; Components:plugin; Flags: ignoreversion recursesubdirs;

[Run]
; The in-app updater passes /RELAUNCH so the app comes back, as the user, once the files are in.
Filename: "{app}\Quarry.exe"; Parameters: "--updated"; Components: mainapp; Flags: nowait runasoriginaluser; Check: RelaunchRequested

[Code]
{ Switches the in-app updater passes (docs/AUTO_UPDATE.md):
    /WAITPID=<pid>  wait for that process (the running Quarry) to exit before touching files
    /RELAUNCH       start Quarry again afterwards, or say why the update was blocked
  Without them this is the same interactive installer as before. }

function OpenProcess(dwDesiredAccess: Cardinal; bInheritHandle: Boolean; dwProcessId: Cardinal): Cardinal;
  external 'OpenProcess@kernel32.dll stdcall';
function WaitForSingleObject(hHandle: Cardinal; dwMilliseconds: Cardinal): Cardinal;
  external 'WaitForSingleObject@kernel32.dll stdcall';
function CloseHandle(hObject: Cardinal): Boolean;
  external 'CloseHandle@kernel32.dll stdcall';
function CreateFileW(lpFileName: String; dwDesiredAccess: Cardinal; dwShareMode: Cardinal;
  lpSecurityAttributes: Cardinal; dwCreationDisposition: Cardinal; dwFlagsAndAttributes: Cardinal;
  hTemplateFile: Cardinal): Cardinal;
  external 'CreateFileW@kernel32.dll stdcall';

const
  SYNCHRONIZE = $00100000;
  GENERIC_WRITE = $40000000;
  OPEN_EXISTING = 3;
  INVALID_HANDLE_VALUE = $FFFFFFFF;
  WAIT_OBJECT_0 = 0;
  WAIT_PROCESS_MS = 60000;

function RelaunchRequested: Boolean;
var
  I: Integer;
begin
  Result := False;
  for I := 1 to ParamCount do
    if CompareText(ParamStr(I), '/RELAUNCH') = 0 then
      Result := True;
end;

{ True once the process is gone (or never existed), False if it outlived the wait. }
function WaitForProcessExit(Pid: Cardinal): Boolean;
var
  H: Cardinal;
begin
  Result := True;
  H := OpenProcess(SYNCHRONIZE, False, Pid);
  if H = 0 then
    Exit; { no such process, or not ours to open: nothing to wait for }
  try
    Result := WaitForSingleObject(H, WAIT_PROCESS_MS) = WAIT_OBJECT_0;
  finally
    CloseHandle(H);
  end;
end;

{ A DLL a host has loaded cannot be opened for writing. We are elevated, so a refusal means
  "in use", not "no permission". }
function FileIsLocked(const FileName: String): Boolean;
var
  H: Cardinal;
begin
  Result := False;
  if not FileExists(FileName) then
    Exit;
  H := CreateFileW(FileName, GENERIC_WRITE, 0, 0, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, 0);
  if H = INVALID_HANDLE_VALUE then
    Result := True
  else
    CloseHandle(H);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Pid: Integer;
  ResultCode: Integer;
begin
  Result := '';

  Pid := StrToIntDef(ExpandConstant('{param:WAITPID|0}'), 0);
  if Pid > 0 then
  begin
    if not WaitForProcessExit(Cardinal(Pid)) then
    begin
      Result := 'Quarry did not close in time, so it was not updated. Close Quarry and run the installer again.';
      Exit;
    end;
  end;

  if WizardIsComponentSelected('plugin') and
     FileIsLocked(ExpandConstant('{commoncf}\VST3\Quarry.vst3\Contents\x86_64-win\Quarry.vst3')) then
  begin
    if RelaunchRequested then
      ExecAsOriginalUser(ExpandConstant('{commonpf}\Quarry\Quarry.exe'), '--update-blocked', '',
        SW_SHOWNORMAL, ewNoWait, ResultCode);
    Result := 'The Quarry VST3 is in use by a host program. Close your DAW and run the installer again.';
  end;
end;
