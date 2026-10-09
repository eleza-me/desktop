; Eleza Desktop — the Windows installer (Inno Setup 6.3 or newer).
;
; Packs a deployed build directory: Eleza.exe with the Qt libraries and plugins
; that windeployqt put next to it, the OpenSSL DLLs and the C++ runtime (see
; docs/BUILDING.md, "Deploying", and the Windows job of .github/workflows/build.yml).
;
;   iscc /DAppVersion=0.1.0 /DSourceDir=C:\src\eleza\dist\Eleza /DOutputDir=C:\src\eleza\dist\installer packaging\windows\eleza.iss
;
; Installs per user by default (no administrator rights); "for all users" can be
; chosen in the first dialog. User data (%APPDATA%\eleza\Eleza) is never touched,
; neither by an upgrade nor by the uninstaller.

#ifndef AppVersion
  #define AppVersion "0.1.0"
#endif
#ifndef SourceDir
  #define SourceDir "..\..\dist\Eleza"
#endif
#ifndef OutputDir
  #define OutputDir "..\..\dist\installer"
#endif

#define AppName "Eleza"
#define AppExeName "Eleza.exe"
#define AppPublisher "eleza"
#define AppUrl "https://eleza.me"

[Setup]
; Identifies the application for upgrades and the uninstaller: never change it.
AppId={{8E5D2B1C-6F4A-4C7E-9B3A-2D1F0A7C5E41}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppPublisherURL={#AppUrl}
AppSupportURL={#AppUrl}
AppUpdatesURL={#AppUrl}
VersionInfoVersion={#AppVersion}
VersionInfoDescription={#AppName} Desktop Setup
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0.17763
OutputDir={#OutputDir}
OutputBaseFilename=Eleza-{#AppVersion}-windows-x64-setup
SetupIconFile=..\..\Eleza\Resources\art\Eleza.ico
UninstallDisplayIcon={app}\{#AppExeName}
UninstallDisplayName={#AppName}
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; An upgrade over a running copy asks to close it (it keeps running in the tray).
CloseApplications=yes
RestartApplications=no

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[CustomMessages]
english.StartupGroup=Startup:
english.AutoStart=Start {#AppName} when I sign in to Windows

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked
Name: "autostart"; Description: "{cm:AutoStart}"; GroupDescription: "{cm:StartupGroup}"; Flags: unchecked

[Files]
Source: "{#SourceDir}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExeName}"; WorkingDir: "{app}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Registry]
; Optional autostart for the current user; removed again by the uninstaller.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "{#AppName}"; ValueData: """{app}\{#AppExeName}"""; Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent
