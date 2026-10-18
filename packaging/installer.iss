; Inno Setup script for obs-multistream-chat
#ifndef MyAppName
  #define MyAppName "OBS Multistream Chat"
#endif
#ifndef MyAppVersion
  #define MyAppVersion "0.4.0"
#endif
#ifndef MyAppPublisher
  #define MyAppPublisher "otissy"
#endif
#ifndef MyAppURL
  #define MyAppURL "https://github.com/otissy/obs-multistream-chat"
#endif

#ifndef SourceTree
  #define SourceTree "D:\builds\obs-multistream-chat\dist\stage\obs-multistream-chat"
#endif
#ifndef OutDir
  #define OutDir "D:\builds\obs-multistream-chat\dist"
#endif

[Setup]
AppId={{5D8C4729-163B-4AEB-9685-648BD1281A02}
AppName={#MyAppName}
AppVersion={#MyAppVersion}
AppPublisher={#MyAppPublisher}
AppPublisherURL={#MyAppURL}
AppSupportURL={#MyAppURL}
AppUpdatesURL={#MyAppURL}
DefaultDirName={commonappdata}\obs-studio\plugins\obs-multistream-chat
DisableDirPage=no
AppendDefaultDirName=no
OutputDir={#OutDir}
OutputBaseFilename=obs-multistream-chat-{#MyAppVersion}-windows-x64-setup
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
PrivilegesRequired=admin
CloseApplications=yes
CloseApplicationsFilter=obs64.exe

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Files]
Source: "{#SourceTree}\bin\64bit\obs-multistream-chat.dll"; DestDir: "{app}\bin\64bit"; Flags: ignoreversion restartreplace
Source: "{#SourceTree}\data\*"; DestDir: "{app}\data"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceTree}\docs\*"; DestDir: "{app}\docs"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#SourceTree}\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceTree}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceTree}\NOTICE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#SourceTree}\CHANGELOG.md"; DestDir: "{app}"; Flags: ignoreversion

[InstallDelete]
; Clean up previous nested bug if present
Type: filesandordirs; Name: "{app}\data\data"
; Clean up old renamed folder if present
Type: filesandordirs; Name: "{commonappdata}\obs-studio\plugins\obs-multistream"
