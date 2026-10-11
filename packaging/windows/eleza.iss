; Eleza Desktop — the Windows installer (Inno Setup 6.3 or newer).
;
; Packs a deployed build directory: Eleza.exe with the Qt libraries and plugins
; that windeployqt put next to it, the OpenSSL DLLs, the C++ runtime, the media
; library of voice calls (livekit_ffi.dll with its notices), Updater.exe
; and eleza-files.list (see docs/BUILDING.md, "Deploying", and the Windows job of
; .github/workflows/build.yml). Run packaging\make-package.py on the directory
; first: it writes eleza-files.list, without which the installed copy would not
; update itself.
;
;   iscc /DAppVersion=0.3.0 /DSourceDir=C:\src\eleza\dist\Eleza /DOutputDir=C:\src\eleza\dist\installer packaging\windows\eleza.iss
;
; Installs for the current user, into %LOCALAPPDATA%\Programs\Eleza, without
; administrator rights: the app updates itself there (docs/RELEASING.md) and
; never has to ask for them. An install for all users (under Program Files) is
; still possible from the command line (/ALLUSERS); such a copy asks for
; administrator rights at every update. User data (%APPDATA%\eleza\Eleza) is
; never touched, neither by an upgrade nor by the uninstaller.

#ifndef AppVersion
  #define AppVersion "0.3.0"
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
; {autopf} is {localappdata}\Programs for the per-user install this is, and
; Program Files only when setup was started with /ALLUSERS.
DefaultDirName={autopf}\{#AppName}
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
; No "for all users?" question: the per-user install is the one that updates
; itself silently. /ALLUSERS and /CURRENTUSER still work.
PrivilegesRequiredOverridesAllowed=commandline
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

; The updater is one of the files above; setup stops when the deployed
; directory was made without it or without the file list.
#if !FileExists(AddBackslash(SourceDir) + "Updater.exe")
  #error Updater.exe is missing from the source directory
#endif
#if !FileExists(AddBackslash(SourceDir) + "eleza-files.list")
  #error eleza-files.list is missing: run packaging\make-package.py on the source directory first
#endif

[InstallDelete]
; What an update that was cut off may have left (the app cleans these up as
; well, see lib/install).
Type: filesandordirs; Name: "{app}\_backup_*"
Type: filesandordirs; Name: "{app}\_update_*"
Type: files; Name: "{app}\_update_journal.txt"

[UninstallDelete]
; Updates bring files the uninstaller's own list does not know.
Type: filesandordirs; Name: "{app}"

[Icons]
Name: "{autoprograms}\{#AppName}"; Filename: "{app}\{#AppExeName}"; WorkingDir: "{app}"
Name: "{autodesktop}\{#AppName}"; Filename: "{app}\{#AppExeName}"; WorkingDir: "{app}"; Tasks: desktopicon

[Registry]
; Optional autostart for the current user; removed again by the uninstaller.
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "{#AppName}"; ValueData: """{app}\{#AppExeName}"""; Flags: uninsdeletevalue; Tasks: autostart

[Run]
Filename: "{app}\{#AppExeName}"; Description: "{cm:LaunchProgram,{#AppName}}"; Flags: nowait postinstall skipifsilent
