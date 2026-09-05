; Inno Setup script. Built by .github/workflows/build.yml:
;   iscc /DArch=x64 /DSrc=..\stage\x64 /DVersion=0.1.0 installer\talktoclaude.iss
;
; Per-user install (no admin), into %LOCALAPPDATA%\Programs\talktoclaude, and
; that folder is added to the user's PATH so `talktoclaude` works from any
; terminal. The model is not bundled; the exe fetches it on first run into
; %LOCALAPPDATA%\talktoclaude\models.

#ifndef Arch
  #define Arch "x64"
#endif
#ifndef Src
  #define Src "..\stage\" + Arch
#endif
#ifndef Version
  #define Version "0.0.0"
#endif

[Setup]
AppId={{7C2B1B0E-6C3B-4F3E-9D3A-talktoclaude}
AppName=talktoclaude
AppVersion={#Version}
AppPublisher=FrenchCommando
AppPublisherURL=https://github.com/FrenchCommando/talktoclaude
DefaultDirName={localappdata}\Programs\talktoclaude
DisableDirPage=yes
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
OutputDir=..\dist
OutputBaseFilename=talktoclaude-{#Version}-windows-{#Arch}-setup
Compression=lzma2
SolidCompression=yes
ChangesEnvironment=yes
UninstallDisplayIcon={app}\talktoclaude.exe
#if Arch == "arm64"
ArchitecturesAllowed=arm64
ArchitecturesInstallIn64BitMode=arm64
#else
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
#endif

[Files]
Source: "{#Src}\talktoclaude.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#Src}\*.dll"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{userprograms}\talktoclaude"; Filename: "{app}\talktoclaude.exe"

[Registry]
; Append {app} to the user's PATH unless it is already there.
Root: HKCU; Subkey: "Environment"; ValueType: expandsz; ValueName: "Path"; \
  ValueData: "{olddata};{app}"; Check: NeedsAddPath(ExpandConstant('{app}'))

[Run]
Filename: "{app}\talktoclaude.exe"; Description: "Run talktoclaude now (downloads the 142 MB model on first run)"; \
  Flags: nowait postinstall skipifsilent

[Code]
function NeedsAddPath(Param: string): boolean;
var
  OrigPath: string;
begin
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', OrigPath) then
  begin
    Result := True;
    exit;
  end;
  Result := Pos(';' + Uppercase(Param) + ';', ';' + Uppercase(OrigPath) + ';') = 0;
end;

// On uninstall, take {app} back out of PATH.
procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var
  OrigPath, App: string;
  P: Integer;
begin
  if CurUninstallStep <> usPostUninstall then exit;
  if not RegQueryStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', OrigPath) then exit;
  App := ExpandConstant('{app}');
  P := Pos(';' + Uppercase(App), Uppercase(OrigPath));
  if P > 0 then
  begin
    Delete(OrigPath, P, Length(App) + 1);
    RegWriteExpandStringValue(HKEY_CURRENT_USER, 'Environment', 'Path', OrigPath);
  end;
end;
