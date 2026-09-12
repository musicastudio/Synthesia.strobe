; Installer for the Casio LK-S250 light cycling plugin (Inno Setup 6). Compile with ISCC after
; building the DLL:
;   iscc /DBuildDir="..\build\Release" installer\Synthesia.strobe.iss
; or run installer\build_installer.ps1.
;
; The whole install is one file, version.dll, copied into Synthesia's own program folder. Synthesia
; imports version.dll and Windows searches the application directory before System32, so the copy
; beside Synthesia.exe loads in place of the system one and forwards every entry point to it. No
; Synthesia file is read, renamed or modified, and uninstalling just deletes the DLL again.

#ifndef BuildDir
  #define BuildDir "..\build\Release"
#endif

[Setup]
AppName=Synthesia LK-S250 Light Cycling
AppVersion=1.0.0
AppPublisher=Musica Studio
AppSupportURL=https://github.com/musicastudio/Synthesia.strobe
DefaultDirName={commonpf32}\Synthesia
DirExistsWarning=no
AppendDefaultDirName=no
DisableProgramGroupPage=yes
PrivilegesRequired=admin
OutputBaseFilename=Synthesia.strobe-Setup
Uninstallable=yes
ArchitecturesInstallIn64BitMode=x64compatible
WizardStyle=modern

[Messages]
; The destination page is Synthesia's own folder, not a folder of ours, so say so plainly.
WizardSelectDir=Select Synthesia's folder
SelectDirDesc=Where is Synthesia installed?
SelectDirLabel3=Setup will copy version.dll into Synthesia's program folder. This is the folder that already holds Synthesia.exe.
SelectDirBrowseLabel=To continue, click Next. If you installed Synthesia somewhere else, click Browse.

[Files]
Source: "{#BuildDir}\version.dll"; DestDir: "{app}"; Flags: ignoreversion

[Code]
// The plugin matches Casio key-light SysEx on its way out of the MIDI API, so it holds no
// Synthesia addresses and is not tied to one build. We still check Synthesia.exe is actually
// there, since the DLL only does anything when Synthesia loads it.
function NextButtonClick(CurPageID: Integer): Boolean;
var exe, dll: string;
begin
  Result := True;
  if CurPageID <> wpSelectDir then exit;

  exe := AddBackslash(WizardDirValue) + 'Synthesia.exe';
  if not FileExists(exe) then
  begin
    Result := (MsgBox('Synthesia.exe was not found in:' + #13#10#13#10 + WizardDirValue + #13#10#13#10 +
                      'The plugin only does anything when Synthesia loads it from its own folder.' + #13#10#13#10 +
                      'Install here anyway?', mbConfirmation, MB_YESNO) = IDYES);
    exit;
  end;

  // Never quietly replace someone else's proxy DLL of the same name.
  dll := AddBackslash(WizardDirValue) + 'version.dll';
  if FileExists(dll) then
    Result := (MsgBox('A version.dll is already in that folder and will be replaced.' + #13#10#13#10 +
                      'If you installed this plugin before, that is expected. If something else put it ' +
                      'there, replacing it may stop that working.' + #13#10#13#10 +
                      'Replace it?', mbConfirmation, MB_YESNO) = IDYES);
end;
