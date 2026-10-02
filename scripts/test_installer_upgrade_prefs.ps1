param(
    [string]$Iscc = "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$source = Get-Content -LiteralPath (Join-Path $repo 'installer/PulseSetup.iss') -Raw
$out = Join-Path $repo 'build/installer-prefs-test'
New-Item -ItemType Directory -Force -Path $out | Out-Null
$prefs = [regex]::Match($source, '(?s)function FolderClass\(.*?(?=function IsChinese:)').Value
$cleanup = [regex]::Match($source, '(?s)procedure DeleteFolderOpenOverride\(.*?(?=\r?\nconst\r?\n)').Value
if (!$prefs -or !$cleanup) { throw 'Installer procedure boundaries changed; update the harness.' }
$code = $prefs + $cleanup
foreach ($name in @('RegQueryStringValue', 'RegWriteStringValue', 'RegDeleteValue', 'RegDeleteKeyIncludingSubkeys', 'ExpandConstant', 'CleanupPulseData', 'UninstallSilent')) {
    $code = $code -replace "\b$name\b", "Mock$name"
}
if ($code -match '\b(Reg\w+|Exec|ShellExec|DeleteFile|DelTree|ExpandConstant|CleanupPulseData)\s*\(') {
    throw 'Unmocked external operation in extracted installer code.'
}
$report = (Join-Path $out 'result.txt').Replace("'", "''")
$prefix = @'
[Setup]
AppName=Pulse preference regression harness
AppVersion=1
DefaultDirName={tmp}\PulsePrefsHarnessNeverInstalled
PrivilegesRequired=lowest
Uninstallable=no
CreateAppDir=no
OutputBaseFilename=installer-prefs-harness
Compression=none
[Code]
var
  UpgradePrefsCaptured, UpgradeStartupPresent, CleanupUserData: Boolean;
  UpgradeStartupCommand, UpgradePreviousExe, AppPath, Report: String;
  UpgradeFolderCommands: array[0..1] of String;
  Keys, Values: array[0..63] of String;
  Count, Failures, DataCleanups: Integer;

function FindKey(const Key, Name: String): Integer;
var I: Integer;
begin
  Result := -1;
  for I := 0 to Count - 1 do
    if CompareText(Keys[I], Key + '|' + Name) = 0 then Result := I;
end;
function MockRegQueryStringValue(Root: Integer; const Key, Name: String; var Value: String): Boolean;
var I: Integer;
begin
  I := FindKey(Key, Name); Result := I >= 0;
  if Result then Value := Values[I];
end;
function MockRegWriteStringValue(Root: Integer; const Key, Name, Value: String): Boolean;
var I: Integer;
begin
  I := FindKey(Key, Name);
  if I < 0 then begin I := Count; Count := Count + 1; end;
  Keys[I] := Key + '|' + Name; Values[I] := Value; Result := True;
end;
function MockRegDeleteValue(Root: Integer; const Key, Name: String): Boolean;
var I: Integer;
begin
  I := FindKey(Key, Name); Result := I >= 0;
  if Result then Keys[I] := '';
end;
function MockRegDeleteKeyIncludingSubkeys(Root: Integer; const Key: String): Boolean;
var I: Integer;
begin
  for I := 0 to Count - 1 do
    if (Pos(Lowercase(Key) + '\', Lowercase(Keys[I])) = 1) or
       (Pos(Lowercase(Key) + '|', Lowercase(Keys[I])) = 1) then Keys[I] := '';
  Result := True;
end;
function MockExpandConstant(const Value: String): String;
begin
  Result := Value; StringChangeEx(Result, '{app}', AppPath, True);
end;
procedure MockCleanupPulseData;
begin DataCleanups := DataCleanups + 1; end;
function MockUninstallSilent: Boolean;
begin
  { This runs in a Setup harness, so explicitly model a silent uninstaller.
    Restoring an erroneous "if not UninstallSilent" guard must fail the test. }
  Result := True;
end;
procedure Check(Condition: Boolean; const LabelText: String);
begin
  if Condition then Report := Report + '[PASS] ' + LabelText + #13#10
  else begin Failures := Failures + 1; Report := Report + '[FAIL] ' + LabelText + #13#10; end;
end;
function ValueAt(const Key, Name: String): String;
begin
  Result := '<missing>'; MockRegQueryStringValue(HKCU, Key, Name, Result);
end;
procedure Reset;
begin
  Count := 0; UpgradePrefsCaptured := False; UpgradeStartupPresent := False;
  UpgradeStartupCommand := ''; UpgradePreviousExe := '';
  UpgradeFolderCommands[0] := ''; UpgradeFolderCommands[1] := '';
  AppPath := 'C:\New Pulse'; CleanupUserData := False; DataCleanups := 0;
end;
'@
$tests = @'
function InitializeSetup: Boolean;
var RunKey, DirKey, DriveKey: String;
begin
  RunKey := 'Software\Microsoft\Windows\CurrentVersion\Run';
  DirKey := 'Software\Classes\Directory\shell';
  DriveKey := 'Software\Classes\Drive\shell';
  Reset;
  MockRegWriteStringValue(HKCU, RunKey, 'Pulse', '"C:\Old Pulse\pulse.exe" --background');
  MockRegWriteStringValue(HKCU, DirKey + '\open\command', '', '"C:\Old Pulse\pulse.exe" "%1"');
  MockRegWriteStringValue(HKCU, DirKey, '', 'open');
  MockRegWriteStringValue(HKCU, DriveKey + '\open\command', '', '"D:\Other\pulse.exe" "%1"');
  MockRegWriteStringValue(HKCU, DriveKey, '', 'open');
  CaptureUpgradePrefs('C:\Old Pulse\pulse.exe');
  Check(UpgradeFolderCommands[1] = '', 'do not capture another installation association');
  Count := 0; { Old uninstaller erased all prior preferences. }
  MockRegWriteStringValue(HKCU, DriveKey + '\open\command', '', '"D:\Other\pulse.exe" "%1"');
  RestoreUpgradePrefs;
  Check(ValueAt(RunKey, 'Pulse') = '"C:\New Pulse\Pulse.exe" --background', 'restore startup after old uninstaller; relocate and preserve arguments');
  Check(ValueAt(DirKey + '\open\command', '') = '"C:\New Pulse\Pulse.exe" "%1"', 'restore owned folder association at new path');
  Check((ValueAt(DirKey, '') = 'open') and (ValueAt(DirKey + '\open', 'DelegateExecute') = ''), 'restore folder default verb and delegate override');
  Check(ValueAt(DriveKey + '\open\command', '') = '"D:\Other\pulse.exe" "%1"', 'leave unrelated folder association untouched');
  Reset;
  MockRegWriteStringValue(HKCU, DirKey + '\open\command', '', '"C:\Old Pulse\pulse.exe" "%1"');
  MockRegWriteStringValue(HKCU, DirKey, '', 'explore');
  CaptureUpgradePrefs('C:\Old Pulse\pulse.exe');
  Count := 0; RestoreUpgradePrefs;
  Check(ValueAt(DirKey + '\open\command', '') = '<missing>', 'do not revive inactive leftover folder override');
  Reset;
  CaptureUpgradePrefs('C:\Old Pulse\pulse.exe');
  MockRegWriteStringValue(HKCU, RunKey, 'Pulse', 'installer task enabled this');
  RestoreUpgradePrefs;
  Check(ValueAt(RunKey, 'Pulse') = '<missing>', 'absent startup value remains disabled despite installer task');
  Reset;
  MockRegWriteStringValue(HKCU, RunKey, 'Pulse', '"C:\New Pulse\Pulse.exe"');
  MockRegWriteStringValue(HKCU, DirKey + '\open\command', '', '"C:\New Pulse\Pulse.exe" "%1"');
  MockRegWriteStringValue(HKCU, DirKey, '', 'open');
  CurUninstallStepChanged(usPostUninstall);
  Check((ValueAt(RunKey, 'Pulse') = '<missing>') and
    (ValueAt(DirKey + '\open\command', '') = '<missing>') and (ValueAt(DirKey, '') = '<missing>'),
    'silent standalone uninstall still removes startup and folder overrides');
  Check(DataCleanups = 0, 'silent uninstall keeps user data when cleanup is disabled');
  CleanupUserData := True; CurUninstallStepChanged(usPostUninstall);
  Check(DataCleanups = 1, 'explicit data cleanup reaches mock only');
  { Returning False prevents installation, file deployment, and uninstall registration. }
  Result := False;
'@
$harness = $prefix + "`r`n" + $code + "`r`n" + $tests + "`r`n  SaveStringToFile('$report', Report, False);`r`nend;`r`n"
$iss = Join-Path $out 'harness.iss'
Set-Content -LiteralPath $iss -Value $harness -Encoding utf8
& $Iscc "/O$out" $iss
if ($LASTEXITCODE -ne 0) { throw 'Pascal harness compilation failed.' }
$exe = Join-Path $out 'installer-prefs-harness.exe'
if (Test-Path -LiteralPath (Join-Path $out 'result.txt')) { Remove-Item -LiteralPath (Join-Path $out 'result.txt') }
$process = Start-Process -FilePath $exe -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -WindowStyle Hidden -Wait -PassThru
if (!(Test-Path -LiteralPath (Join-Path $out 'result.txt'))) { throw "Harness produced no report (exit $($process.ExitCode))." }
$result = Get-Content -LiteralPath (Join-Path $out 'result.txt') -Raw
Write-Output $result
if ($result -match '\[FAIL\]') { throw 'Installer preference regression failed.' }
if (($result -split '\[PASS\]').Count -ne 11) { throw 'Unexpected number of completed assertions.' }
