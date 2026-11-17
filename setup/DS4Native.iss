; DS4-Native installer (Inno Setup 6+). Built by package.bat, which also downloads the two
; driver installers it bundles into setup\redist:
;   ViGEmBus (BSD-3-Clause) - required, for the virtual Xbox controllers
;   HidHide  (MIT)          - optional, hides the real controller from games
; Both are Nefarius' own signed installers, run silently, and only when not installed already.

#ifndef AppVer
  #define AppVer "1.1.0"
#endif
#define ViGEmSetup "ViGEmBus_1.22.0_x64_x86_arm64.exe"
#define HidHideSetup "HidHide_1.5.230_x64.exe"

[Setup]
AppId={{C6A5326A-078E-49EE-83E8-3FA92E67E30A}
AppName=DS4-Native
AppVersion={#AppVer}
AppVerName=DS4-Native {#AppVer}
AppPublisher=toasty-wheeler
AppPublisherURL=https://github.com/toasty-wheeler/ds4-native
AppSupportURL=https://github.com/toasty-wheeler/ds4-native/issues
DefaultDirName={autopf}\DS4-Native
DisableProgramGroupPage=yes
OutputDir=..\dist
OutputBaseFilename=DS4-Native-Setup-{#AppVer}
Compression=lzma2/ultra
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
UninstallDisplayIcon={app}\DS4Native.exe
UninstallDisplayName=DS4-Native
WizardStyle=modern
CloseApplications=no

[Tasks]
Name: hidhide; Description: "Install HidHide, so games only see the Xbox controller (recommended)"; Check: not HidHideInstalled
Name: startup; Description: "Start DS4-Native with Windows"; Flags: unchecked
Name: desktopicon; Description: "Create a desktop shortcut"; Flags: unchecked

[Files]
Source: "..\app\DS4Native.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "redist\{#ViGEmSetup}"; DestDir: "{tmp}"; Flags: deleteafterinstall; Check: not ViGEmInstalled
Source: "redist\{#HidHideSetup}"; DestDir: "{tmp}"; Flags: deleteafterinstall; Tasks: hidhide

[Icons]
Name: "{autoprograms}\DS4-Native"; Filename: "{app}\DS4Native.exe"
Name: "{autodesktop}\DS4-Native"; Filename: "{app}\DS4Native.exe"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "DS4-Native"; \
    ValueData: """{app}\DS4Native.exe"" /tray"; Tasks: startup

[Run]
Filename: "{app}\DS4Native.exe"; Description: "Start DS4-Native"; Flags: nowait postinstall skipifsilent

[Code]
var
  Restart: Boolean;

function ServiceInstalled(Name: String): Boolean;
begin
  Result := RegKeyExists(HKLM, 'SYSTEM\CurrentControlSet\Services\' + Name);
end;

function ViGEmInstalled: Boolean;
begin
  Result := ServiceInstalled('ViGEmBus');
end;

function HidHideInstalled: Boolean;
begin
  Result := ServiceInstalled('HidHide');
end;

// Closes a running copy the normal way (it shows the controllers it hid again).
procedure CloseRunningCopy;
var
  Code: Integer;
begin
  if FileExists(ExpandConstant('{app}\DS4Native.exe')) then
    Exec(ExpandConstant('{app}\DS4Native.exe'), '/exit', '', SW_HIDE, ewWaitUntilTerminated, Code);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
begin
  CloseRunningCopy;
  Result := '';
end;

procedure RunDriverSetup(FileName, Title: String);
var
  Code: Integer;
begin
  WizardForm.StatusLabel.Caption := 'Installing ' + Title + '...';
  if not Exec(ExpandConstant('{tmp}\' + FileName), '/exenoui /qn /norestart', '', SW_HIDE, ewWaitUntilTerminated, Code) then
    MsgBox(Title + ' could not be started. Install it from github.com/nefarius.', mbError, MB_OK)
  else if (Code = 3010) or (Code = 1641) then
    Restart := True
  else if Code <> 0 then
    MsgBox(Title + ' setup ended with code ' + IntToStr(Code) + '.', mbError, MB_OK);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then begin
    if FileExists(ExpandConstant('{tmp}\{#ViGEmSetup}')) then RunDriverSetup('{#ViGEmSetup}', 'ViGEmBus');
    if FileExists(ExpandConstant('{tmp}\{#HidHideSetup}')) then RunDriverSetup('{#HidHideSetup}', 'HidHide');
  end;
end;

function NeedRestart: Boolean;
begin
  Result := Restart;
end;

// The drivers stay (other apps use them too); DS4-Native is closed first and its
// start-with-Windows entry removed.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then begin
    CloseRunningCopy;
    RegDeleteValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Run', 'DS4-Native');
  end;
end;
