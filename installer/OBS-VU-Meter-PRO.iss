#define MyAppName "OBS VU Meter PRO"
#define MyAppVersion "1.1.0"
#define MyAppPublisher "Doctor DJ"
#define MyAppURL "https://github.com/doctordj/obs-vu-meter-pro"
#define PluginFolder "obs-vu-meter-pro"

[Setup]
AppId={{A7D8B2C4-96A1-4E5F-9B17-33A0B4C5D6E7}}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
DefaultDirName={commonappdata}\obs-studio\plugins\{#PluginFolder}
DisableProgramGroupPage=yes
PrivilegesRequired=admin
ArchitecturesInstallIn64BitMode=x64
OutputDir=output
OutputBaseFilename=OBS-VU-Meter-PRO-OBS33-Setup
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#MyAppName}

[Files]
Source: "..\staging\obs-vu-meter-pro.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\staging\data\*"; DestDir: "{app}\data"; Flags: ignoreversion recursesubdirs createallsubdirs

[InstallDelete]
; Remove earlier copies from legacy install locations to avoid duplicate modules.
Type: files; Name: "{autopf}\obs-studio\obs-plugins\64bit\obs-vu-meter-pro.dll"
Type: filesandordirs; Name: "{autopf}\obs-studio\data\obs-plugins\obs-vu-meter-pro"
Type: filesandordirs; Name: "{commonappdata}\obs-studio\plugins\obs-vu-meter-pro\bin"

[UninstallDelete]
Type: filesandordirs; Name: "{app}\data"
