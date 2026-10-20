[Code]
// Unpacks one downloaded voice pack (<id>.tar.bz2 in {tmp}) into {app}\data\voices.
// Windows' own tar.exe cannot read bzip2 ("unable to run program bzip2 -d") on a PC without a bzip2
// on PATH, so the bzip2 layer is undone by Setup's 7-Zip (needs ArchiveExtraction=full, Inno Setup
// 6.5+) and tar.exe only unpacks the plain .tar that comes out of it.
// #included inside installer.iss's [Code] and by the packaging test harness, so both run this.
// Pascal comments only: a ';' line here would be read as code, not as a comment.
function UnpackVoiceArchive(const Archive, Dest: String; var Err: String): Boolean;
var
  Work, TarFile: String;
  Code: Integer;
begin
  Result := False;
  Work := Archive + '.unpack';
  DelTree(Work, True, True, True);
  ForceDirectories(Work);
  ForceDirectories(Dest);
  try
    try
      ExtractArchive(Archive, Work, '', True, nil);
    except
      Err := 'bzip2: ' + GetExceptionMessage;
      Exit;
    end;
    TarFile := Work + '\' + ChangeFileExt(ExtractFileName(Archive), '');
    if not FileExists(TarFile) then begin
      Err := 'no ' + ExtractFileName(TarFile) + ' inside the archive';
      Exit;
    end;
    if not Exec(ExpandConstant('{sys}\tar.exe'), '-xf "' + TarFile + '" -C "' + Dest + '"', '', SW_HIDE,
                ewWaitUntilTerminated, Code) or (Code <> 0) then begin
      Err := 'tar exit ' + IntToStr(Code);
      Exit;
    end;
    Result := True;
  finally
    DelTree(Work, True, True, True);
  end;
end;

procedure ExtractVoicePack(const BaseName: String);
var
  Archive, Err: String;
begin
  Archive := ExpandConstant('{tmp}\' + BaseName);
  if not FileExists(Archive) then
    Exit;
  if UnpackVoiceArchive(Archive, ExpandConstant('{app}\data\voices'), Err) then
    Log('Voice pack ' + BaseName + ' unpacked.')
  else begin
    Log('Voice pack ' + BaseName + ' could not be unpacked: ' + Err);
    SuppressibleMsgBox('The voice pack ' + BaseName + ' could not be unpacked (' + Err +
                       '). Its voices will be missing; run Setup again to retry.', mbError, MB_OK, IDOK);
  end;
  DeleteFile(Archive);
end;
