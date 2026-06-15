; EARS Bridge - Windows installer (Inno Setup 6)
; Build with:  tools\build-installer.cmd   (configures + builds Release, then compiles this)
; or directly: "ISCC.exe" installer\ears-bridge.iss
;
; Paths below are relative to THIS file (the installer\ folder).

#define MyAppName "EARS Bridge"
#ifndef MyAppVersion
  #define MyAppVersion "0.2.2"
#endif
#define MyAppPublisher "Elevatormusic"
#define MyAppURL "https://github.com/Elevatormusic/ears-bridge"
#define MyAppExeName "EARS Bridge.exe"

; Folder holding the built app exe. Override with ISCC /DBuildDir=...
#ifndef BuildDir
  #define BuildDir "..\build\EarsBridge_artefacts\Release"
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif

[Setup]
; AppId uniquely identifies this app for upgrades/uninstall - keep it STABLE across versions.
AppId={{8FB12BAD-05F5-4D64-B021-9E1EB612FC28}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppVerName={#MyAppName} {#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={autopf}\{#MyAppName}
DefaultGroupName={#MyAppName}
DisableProgramGroupPage=yes
UninstallDisplayIcon={app}\{#MyAppExeName}
UninstallDisplayName={#MyAppName} {#MyAppVersion}
OutputDir={#OutputDir}
OutputBaseFilename=EARS-Bridge-{#MyAppVersion}-Setup
SetupIconFile=assets\icon.ico
InfoBeforeFile=prerequisites.txt
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
; Branded wizard art matching the app icon (system-blue headphones).
WizardImageFile=assets\wizard-large.bmp
WizardSmallImageFile=assets\wizard-small.bmp
; The app is built x64; install into the 64-bit Program Files and only on x64-capable Windows.
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
; Allow a per-user install if the user has no admin rights.
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "{cm:CreateDesktopIcon}"; GroupDescription: "{cm:AdditionalIcons}"; Flags: unchecked

[Files]
Source: "{#BuildDir}\{#MyAppExeName}"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"
Name: "{group}\{cm:UninstallProgram,{#MyAppName}}"; Filename: "{uninstallexe}"
Name: "{autodesktop}\{#MyAppName}"; Filename: "{app}\{#MyAppExeName}"; Tasks: desktopicon

[Run]
Filename: "{app}\{#MyAppExeName}"; Description: "{cm:LaunchProgram,{#StringChange(MyAppName, '&', '&&')}}"; Flags: nowait postinstall skipifsilent
