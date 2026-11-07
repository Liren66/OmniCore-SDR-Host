#ifndef StageDir
  #error StageDir must be provided by build-package.ps1
#endif
#ifndef OutputDir
  #define OutputDir "..\dist"
#endif
#ifndef AppVersion
  #define AppVersion "0.7.1"
#endif

[Setup]
AppId={{CF2F49E0-0391-4CC4-9576-D789268DDA92}
AppName=OmniCore Host
AppVersion={#AppVersion}
AppPublisher=OmniCoreSDR
AppComments=OmniCoreSDR C++ host runtime and command-line tools
DefaultDirName={autopf}\OmniCore Host
DefaultGroupName=OmniCore Host
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64
ArchitecturesInstallIn64BitMode=x64
MinVersion=10.0
PrivilegesRequired=admin
OutputDir={#OutputDir}
OutputBaseFilename=OmniCoreHost-{#AppVersion}-win64-setup
Compression=lzma2/max
SolidCompression=yes
WizardStyle=modern
UninstallDisplayIcon={app}\bin\och.exe
ChangesEnvironment=yes
SetupLogging=yes
CloseApplications=yes
RestartApplications=no
AllowNoIcons=yes
VersionInfoVersion={#AppVersion}.0

[Tasks]
Name: "driver"; Description: "Install/check libusbK for OmniCoreSDR (USB 1A86:5537 only)"; Flags: checkedonce
Name: "path"; Description: "Add OmniCore Host commands to the system PATH"; Flags: checkedonce

[Files]
; Explicit host payload roots keep legacy SDK/firmware directories out of releases.
Source: "{#StageDir}\bin\*"; DestDir: "{app}\bin"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\include\*"; DestDir: "{app}\include"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\lib\*"; DestDir: "{app}\lib"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\docs\*"; DestDir: "{app}\docs"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\examples\*"; DestDir: "{app}\examples"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\licenses\*"; DestDir: "{app}\licenses"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageDir}\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\LICENSE"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageDir}\PAYLOAD_SHA256.txt"; DestDir: "{app}"; Flags: ignoreversion

[Icons]
Name: "{group}\OmniCore Host terminal"; Filename: "{cmd}"; Parameters: "/K """"{app}\bin\och-shell.cmd"""""; WorkingDir: "{app}"
Name: "{group}\Install OmniCoreSDR driver"; Filename: "{cmd}"; Parameters: "/K """"{app}\bin\och-driver.exe"" install"""; WorkingDir: "{app}"
Name: "{group}\Installation guide"; Filename: "{app}\docs\INSTALL_WINDOWS.txt"
Name: "{group}\Uninstall OmniCore Host"; Filename: "{uninstallexe}"

[Code]
const
  EnvKey = 'SYSTEM\CurrentControlSet\Control\Session Manager\Environment';
var
  DriverResult: Integer;
  DriverAttempted: Boolean;

function PathContains(const Value, Item: String): Boolean;
begin
  Result := Pos(';' + Uppercase(Item) + ';', ';' + Uppercase(Value) + ';') > 0;
end;

procedure AddPath;
var
  Value, Item: String;
begin
  if not WizardIsTaskSelected('path') then Exit;
  Item := ExpandConstant('{app}\bin');
  if not RegQueryStringValue(HKLM, EnvKey, 'Path', Value) then begin
    Log('Could not read system PATH. It was not changed. Use the Start Menu terminal.');
    Exit;
  end;
  if not PathContains(Value, Item) then begin
    if (Length(Value)>0) and (Value[Length(Value)] <> ';') then Value:=Value+';';
    if not RegWriteExpandStringValue(HKLM, EnvKey, 'Path', Value+Item) then
      Log('Could not add OmniCore Host to PATH. Use the Start Menu terminal.');
  end;
end;

procedure RemovePath;
var
  Value, NewValue, Token, Item: String;
  P: Integer;
begin
  Item := Uppercase(ExpandConstant('{app}\bin'));
  if not RegQueryStringValue(HKLM, EnvKey, 'Path', Value) then Exit;
  NewValue := '';
  while Length(Value)>0 do begin
    P:=Pos(';',Value);
    if P=0 then begin Token:=Value; Value:=''; end
    else begin Token:=Copy(Value,1,P-1); Delete(Value,1,P); end;
    if Uppercase(Token)<>Item then begin
      if Length(NewValue)>0 then NewValue:=NewValue+';';
      NewValue:=NewValue+Token;
    end;
  end;
  RegWriteExpandStringValue(HKLM, EnvKey, 'Path', NewValue);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep=ssPostInstall then begin
    AddPath;
    DriverAttempted := WizardIsTaskSelected('driver');
    if DriverAttempted then begin
      WizardForm.StatusLabel.Caption := 'Checking OmniCoreSDR USB driver...';
      if not Exec(ExpandConstant('{app}\bin\och-driver.exe'), 'install', ExpandConstant('{app}\bin'), SW_HIDE, ewWaitUntilTerminated, DriverResult) then DriverResult:=4;
      Log(Format('OmniCoreSDR driver helper result: %d', [DriverResult]));
    end;
  end;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID=wpFinished then begin
    if DriverAttempted and (DriverResult=2) then
      WizardForm.FinishedLabel.Caption := 'OmniCore Host is installed. The SDR data port is not connected. After connecting it, run och-driver install from the OmniCore Host terminal. No Internet connection is required.'
    else if DriverAttempted and (DriverResult<>0) then
      WizardForm.FinishedLabel.Caption := 'OmniCore Host software is installed, but the USB driver could not be confirmed. Open the OmniCore Host terminal and run och-driver install. See docs\INSTALL_WINDOWS.txt and the driver install log. Windows security settings have not been changed.'
    else
      WizardForm.FinishedLabel.Caption := 'OmniCore Host is installed. Open a NEW terminal and run och find, then och info. The Start Menu also contains an OmniCore Host terminal and driver assistant.';
  end;
end;

function GetCustomSetupExitCode: Integer;
begin
  Result := 0;
  if DriverAttempted and (DriverResult<>0) and (DriverResult<>2) then Result:=100+DriverResult;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep=usPostUninstall then RemovePath;
  { The shared libusbK driver, its catalog, and driver certificates intentionally remain installed. }
end;
