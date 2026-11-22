; archwindows installer (Inno Setup 6). Build with installer\build-installer.ps1, which stages the
; SecureUxTheme MSIs and visual styles under build\installer\ and passes the defines below.

#ifndef AppVersion
  #define AppVersion "0.0.0"
#endif
#ifndef UxtVersion
  #define UxtVersion "4.0.0"
#endif
#ifndef Styles
  #define Styles ""
#endif

[Setup]
AppId={{8C5D424A-25A8-4DCF-91E5-54EC805DD2DC}
AppName=archwindows
AppVersion={#AppVersion}
AppVerName=archwindows {#AppVersion}
AppPublisher=archwindows
SourceDir=..
OutputDir=build
OutputBaseFilename=archwindows-setup-{#AppVersion}
DefaultDirName={autopf}\archwindows
DisableProgramGroupPage=yes
; SecureUxTheme writes to System32 and HKLM, so the whole install is per-machine.
PrivilegesRequired=admin
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
UninstallDisplayIcon={app}\archwindows.exe
SetupIconFile=src\archwindows.ico
UninstallDisplayName=archwindows
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
SetupLogging=yes
; HKCU entries (Run / RunOnce) go to the account that elevated, which is the signed-in user
; under normal UAC.
UsedUserAreasWarning=no

[Tasks]
Name: uxtheme;     Description: "Install SecureUxTheme {#UxtVersion} (lets Windows load the bundled visual styles; needs a restart)"; Check: OfferUxt
Name: startup;     Description: "Start archwindows when I sign in"
Name: desktopicon; Description: "Create a desktop shortcut"; Flags: unchecked

[Files]
Source: "build\archwindows.exe";                       DestDir: "{app}"; Flags: ignoreversion
Source: "assets\*";                                    DestDir: "{app}\assets"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "build\installer\styles\*";                    DestDir: "{app}\msstyles"; Flags: ignoreversion recursesubdirs createallsubdirs skipifsourcedoesntexist
Source: "LICENSE";                                     DestDir: "{app}"; DestName: "LICENSE.txt"
Source: "README.md";                                   DestDir: "{app}"
Source: "installer\THIRD-PARTY-NOTICES.txt";           DestDir: "{app}"
Source: "build\installer\deps\SecureUxTheme-COPYING.txt"; DestDir: "{app}\licenses"
Source: "build\installer\deps\SecureUxTheme_x64.msi";   Flags: dontcopy
Source: "build\installer\deps\SecureUxTheme_ARM64.msi"; Flags: dontcopy

[Icons]
Name: "{autoprograms}\archwindows"; Filename: "{app}\archwindows.exe"
Name: "{autodesktop}\archwindows";  Filename: "{app}\archwindows.exe"; Tasks: desktopicon

[Registry]
Root: HKCU; Subkey: "Software\Microsoft\Windows\CurrentVersion\Run"; ValueType: string; ValueName: "archwindows"; ValueData: """{app}\archwindows.exe"""; Flags: uninsdeletevalue; Tasks: startup

[Run]
Filename: "{app}\archwindows.exe"; Description: "Start archwindows now"; Flags: nowait postinstall skipifsilent runasoriginaluser

[UninstallRun]
; Stop the shell, then let a fresh --reset put back title bars / colours and unhide workspace windows.
Filename: "{sys}\taskkill.exe";    Parameters: "/f /im archwindows.exe"; Flags: runhidden; RunOnceId: "KillShell"
Filename: "{app}\archwindows.exe"; Parameters: "--reset";               Flags: runhidden waituntilterminated; RunOnceId: "ResetWindows"

[UninstallDelete]
Type: files; Name: "{app}\SecureUxTheme-install.log"

[Code]
const
  RunOnceKey = 'Software\Microsoft\Windows\CurrentVersion\RunOnce';
  RunOnceVal = 'ArchWindowsStyle';
  UninstKey  = 'SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall';

var
  StylePage: TInputOptionWizardPage;
  StyleNames: TArrayOfString;
  PatcherActive: Boolean;   { a working patcher was already there when setup started }
  UxtLegacy: Boolean;       { SecureUxTheme 3.x: blocks the 4.x MSI and needs per-style patching }
  UxtInstalledNow: Boolean;
  RebootNeeded: Boolean;

{ ---- helpers ------------------------------------------------------------------------------ }

procedure SplitStyles(const S: String);
var Rest: String; P, N: Integer;
begin
  N := 0;
  Rest := S;
  while Rest <> '' do begin
    P := Pos(';', Rest);
    if P = 0 then P := Length(Rest) + 1;
    if P > 1 then begin
      SetArrayLength(StyleNames, N + 1);
      StyleNames[N] := Copy(Rest, 1, P - 1);
      N := N + 1;
    end;
    Delete(Rest, 1, P);
  end;
end;

function ArpHasIn(RootKey: Integer; const Pattern: String): Boolean;
var Keys: TArrayOfString; I: Integer; Name: String;
begin
  Result := False;
  if not RegGetSubkeyNames(RootKey, UninstKey, Keys) then exit;
  for I := 0 to GetArrayLength(Keys) - 1 do
    if RegQueryStringValue(RootKey, UninstKey + '\' + Keys[I], 'DisplayName', Name) then
      if Pos(Pattern, Lowercase(Name)) > 0 then begin
        Result := True;
        exit;
      end;
end;

function ArpHas(const Pattern: String): Boolean;
begin
  Result := ArpHasIn(HKLM64, Pattern) or ArpHasIn(HKLM32, Pattern);
end;

function StyleDir(const Name: String): String;
begin
  Result := ExpandConstant('{app}\msstyles\') + Name + '\';
end;

function OfferUxt: Boolean;
begin
  Result := not PatcherActive and not UxtLegacy;
end;

{ ---- setup events ------------------------------------------------------------------------- }

function InitializeSetup: Boolean;
begin
  UxtLegacy := FileExists(ExpandConstant('{sys}\SecureUxTheme.dll'));
  PatcherActive := FileExists(ExpandConstant('{sys}\SecureUxTheme4.dll')) or ArpHas('ultrauxthemepatcher');
  if UxtLegacy then
    SuppressibleMsgBox('An older SecureUxTheme (3.x) is installed. It needs every visual style patched with its ThemeTool, ' +
      'and it blocks SecureUxTheme {#UxtVersion} from installing.' + #13#10#13#10 +
      'Setup will continue without installing SecureUxTheme. To use the bundled visual styles, uninstall the old version ' +
      'with its ThemeTool, then run this setup again.', mbInformation, MB_OK, IDOK);
  Result := True;
end;

procedure InitializeWizard;
var I: Integer; Msg: String;
begin
  SplitStyles('{#Styles}');
  if GetArrayLength(StyleNames) = 0 then exit;

  if PatcherActive then
    Msg := 'A visual-style patcher is already active, so the style is applied as soon as setup finishes.'
  else
    Msg := 'Windows only loads these styles while SecureUxTheme is active. If setup installs it now, ' +
           'the style is applied the first time you sign in after restarting.';
  StylePage := CreateInputOptionPage(wpSelectTasks, 'Visual style',
    'Choose a window style to apply',
    'These styles recolour buttons, scroll bars, menus and other standard controls to match an archwindows theme. ' + Msg,
    True, False);
  StylePage.Add('Keep my current Windows theme');
  for I := 0 to GetArrayLength(StyleNames) - 1 do
    StylePage.Add('Arch ' + StyleNames[I]);
  StylePage.SelectedValueIndex := 1;
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var RC: Integer;
begin
  { Upgrading over a running shell: stop it and let the old exe undo its window changes. }
  if Exec(ExpandConstant('{sys}\taskkill.exe'), '/f /im archwindows.exe', '', SW_HIDE, ewWaitUntilTerminated, RC)
     and (RC = 0) and FileExists(ExpandConstant('{app}\archwindows.exe')) then
    Exec(ExpandConstant('{app}\archwindows.exe'), '--reset', '', SW_HIDE, ewWaitUntilTerminated, RC);
  Result := '';
end;

{ The staged .theme files say Path=__ARCH_MSSTYLES__; point them at the installed .msstyles. }
procedure FixThemeFiles;
var I: Integer; F: String; A: AnsiString; S: String;
begin
  for I := 0 to GetArrayLength(StyleNames) - 1 do begin
    F := StyleDir(StyleNames[I]) + StyleNames[I] + '.theme';
    if LoadStringFromFile(F, A) then begin
      S := String(A);
      StringChangeEx(S, '__ARCH_MSSTYLES__', StyleDir(StyleNames[I]) + StyleNames[I] + '.msstyles', True);
      SaveStringToFile(F, AnsiString(S), False);
    end;
  end;
end;

procedure InstallUxt;
var Msi, MsiLog: String; RC: Integer;
begin
  if IsArm64 then Msi := 'SecureUxTheme_ARM64.msi' else Msi := 'SecureUxTheme_x64.msi';
  ExtractTemporaryFile(Msi);
  MsiLog := ExpandConstant('{app}\SecureUxTheme-install.log');
  WizardForm.StatusLabel.Caption := 'Installing SecureUxTheme {#UxtVersion}...';
  if not Exec(ExpandConstant('{sys}\msiexec.exe'),
              '/i "' + ExpandConstant('{tmp}\') + Msi + '" /qn /norestart /l*v "' + MsiLog + '"',
              '', SW_HIDE, ewWaitUntilTerminated, RC) then
    RC := -1;
  Log('SecureUxTheme msiexec exit code: ' + IntToStr(RC));
  if (RC = 0) or (RC = 3010) or (RC = 1641) then begin
    UxtInstalledNow := True;
    RebootNeeded := True;   { its hook loads into winlogon at the next boot }
  end else
    SuppressibleMsgBox('SecureUxTheme could not be installed (msiexec exit code ' + IntToStr(RC) + ').' + #13#10 +
      'archwindows itself is installed. Details: ' + MsiLog, mbError, MB_OK, IDOK);
end;

procedure ApplyStyle;
var Idx, RC: Integer; Theme: String;
begin
  if StylePage = nil then exit;
  Idx := StylePage.SelectedValueIndex;
  if Idx <= 0 then exit;
  Theme := StyleDir(StyleNames[Idx - 1]) + StyleNames[Idx - 1] + '.theme';

  if PatcherActive then
    { Same as double-clicking the .theme, in the signed-in user's session. }
    ShellExecAsOriginalUser('open', Theme, '', '', SW_SHOWNORMAL, ewNoWait, RC)
  else if UxtInstalledNow then
    RegWriteStringValue(HKCU, RunOnceKey, RunOnceVal, 'rundll32.exe shell32.dll,ShellExec_RunDLL "' + Theme + '"')
  else
    SuppressibleMsgBox('The visual style was not applied: no visual-style patcher is active, and Windows rejects ' +
      'unsigned styles without one.' + #13#10#13#10 + 'Once a patcher is installed, open:' + #13#10 + Theme,
      mbInformation, MB_OK, IDOK);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then begin
    FixThemeFiles;
    if WizardIsTaskSelected('uxtheme') then InstallUxt;
    ApplyStyle;
  end;
end;

function NeedRestart: Boolean;
begin
  Result := RebootNeeded;
end;

{ ---- uninstall ---------------------------------------------------------------------------- }

function UsesOurStyle: Boolean;
var App, V: String;
begin
  Result := False;
  App := Lowercase(ExpandConstant('{app}\'));
  if RegQueryStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\ThemeManager', 'DllName', V) then
    if Pos(App, Lowercase(V)) > 0 then Result := True;
  if RegQueryStringValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Themes', 'CurrentTheme', V) then
    if Pos(App, Lowercase(V)) > 0 then Result := True;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
var Light: Cardinal; Stock: String; RC: Integer;
begin
  if CurUninstallStep = usUninstall then begin
    RegDeleteValue(HKCU, RunOnceKey, RunOnceVal);
    { Never leave Windows on a style whose files are about to be deleted. }
    if UsesOurStyle then begin
      Stock := ExpandConstant('{win}\Resources\Themes\dark.theme');
      if RegQueryDWordValue(HKCU, 'Software\Microsoft\Windows\CurrentVersion\Themes\Personalize', 'AppsUseLightTheme', Light)
         and (Light = 1) then
        Stock := ExpandConstant('{win}\Resources\Themes\aero.theme');
      if ShellExec('open', Stock, '', '', SW_SHOWNORMAL, ewNoWait, RC) then
        Sleep(2000);
    end;
  end;
  if (CurUninstallStep = usPostUninstall) and not UninstallSilent
     and FileExists(ExpandConstant('{sys}\SecureUxTheme4.dll')) then
    MsgBox('SecureUxTheme was left installed: it is a separate product and other visual styles may rely on it. ' +
      'To remove it as well, uninstall "SecureUxTheme" from Settings > Apps > Installed apps.',
      mbInformation, MB_OK);
end;
