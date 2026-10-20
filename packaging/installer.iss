; Inno Setup script for obs-multistream-chat
#ifndef MyAppName
  #define MyAppName "OBS Multistream Chat"
#endif
#ifndef MyAppVersion
  #define MyAppVersion "0.4.1"
#endif
#ifndef MyAppPublisher
  #define MyAppPublisher "otissy"
#endif
#ifndef MyAppURL
  #define MyAppURL "https://github.com/otissy/obs-multistream-chat"
#endif

; ArchiveExtraction=full (voice-extract.iss) arrived in Inno Setup 6.5
#if Ver < EncodeVer(6,5,0)
  #error Inno Setup 6.5 or newer is needed (ArchiveExtraction=full unpacks the voice packs)
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
ArchiveExtraction=full

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

; ---- chat voices --------------------------------------------------------------------------------
; The voice helper and its runtime (sherpa-onnx) are in data\voice. Voice packs go to data\voices:
; some are included in this setup, the rest are components that are downloaded when ticked (each
; checked against the SHA-256 that scripts/fetch-voices.js pinned after its licence check), then
; unpacked by voice-extract.iss (Setup's 7-Zip for the bzip2 layer, then tar.exe). scripts/voice-installer.js
; writes that list.
[Code]
var
  VoiceDownloads: TDownloadWizardPage;
  VoiceDownloadCount: Integer;

function OnVoiceDownloadProgress(const Url, FileName: String; const Progress, ProgressMax: Int64): Boolean;
begin
  Result := True;
end;

procedure AddVoiceDownload(const Url, BaseName, Sha256: String);
begin
  VoiceDownloads.Add(Url, BaseName, Sha256);
  VoiceDownloadCount := VoiceDownloadCount + 1;
end;

#include "voice-extract.iss"

#if FileExists(AddBackslash(SourcePath) + "voices.generated.iss")
  #include "voices.generated.iss"
#else
[Code]
procedure ForEachVoicePack(Download: Boolean);
begin
end;
#endif

[Code]
procedure InitializeWizard;
begin
  VoiceDownloads := CreateDownloadPage(SetupMessage(msgWizardPreparing), 'Downloading the voice packs you ticked',
                                       @OnVoiceDownloadProgress);
end;

function NextButtonClick(CurPageID: Integer): Boolean;
begin
  Result := True;
  if CurPageID <> wpReady then
    Exit;
  VoiceDownloads.Clear;
  VoiceDownloadCount := 0;
  ForEachVoicePack(True);
  if VoiceDownloadCount = 0 then
    Exit;
  VoiceDownloads.Show;
  try
    try
      VoiceDownloads.Download;
    except
      if VoiceDownloads.AbortedByUser then
        Log('Voice pack download cancelled.')
      else
        SuppressibleMsgBox(AddPeriod(GetExceptionMessage), mbCriticalError, MB_OK, IDOK);
      Result := False;
    end;
  finally
    VoiceDownloads.Hide;
  end;
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
    ForEachVoicePack(False);
end;
