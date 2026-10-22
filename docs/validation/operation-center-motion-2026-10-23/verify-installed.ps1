$ErrorActionPreference = 'Stop'
$root = 'D:\Applications\Pulse'
$package = Get-Content -LiteralPath "$PSScriptRoot\package-audit.json" -Raw | ConvertFrom-Json
$before = Get-Content -LiteralPath "$PSScriptRoot\before-replacement.json" -Raw | ConvertFrom-Json
$preferences = @(Get-Content -LiteralPath "$PSScriptRoot\preferences-before.json" -Raw | ConvertFrom-Json)
$env:PULSE_TEST_HIDDEN_SHOT = '1'
$env:PULSE_TEST_DATA_DIR = 'E:\Projects\pulse\bench_data\operation-center-motion-installed-profile'
$shot = Join-Path $PSScriptRoot 'installed-smoke.png'
$fixture = 'E:\Projects\pulse\bench_data\release-1.2.0.0\fixture'
$process = Start-Process -FilePath "$root\Pulse.exe" -WorkingDirectory $root -WindowStyle Hidden -ArgumentList @('--test-instance','--shot',$shot,$fixture) -PassThru
if (-not $process.WaitForExit(30000)) { throw 'Installed smoke timed out; process was left running for inspection' }
if ($process.ExitCode -ne 0 -or -not (Test-Path -LiteralPath $shot)) { throw "Installed smoke failed: $($process.ExitCode)" }
foreach ($file in $package.Files) {
    if ((Get-FileHash -LiteralPath (Join-Path $root $file.Path) -Algorithm SHA256).Hash -ne $file.SHA256) { throw "Payload mismatch: $($file.Path)" }
}
foreach ($file in $preferences) {
    if ((Get-FileHash -LiteralPath $file.Path -Algorithm SHA256).Hash -ne $file.SHA256) { throw "Preference changed: $($file.Path)" }
}
$service = Get-CimInstance Win32_Service -Filter "Name='PulseIndex'" | Select-Object Name,State,StartMode,PathName,ProcessId
foreach ($property in @('State','StartMode','PathName','ProcessId')) {
    if ($service.$property -ne $before.Service.$property) { throw "Service changed: $property" }
}
$processes = @(Get-CimInstance Win32_Process | Where-Object { $_.Name -like 'Pulse*' } | Select-Object ProcessId,Name,ExecutablePath,CommandLine)
if (@(Compare-Object @($before.Processes.ProcessId | Sort-Object) @($processes.ProcessId | Sort-Object)).Count) { throw 'Remaining Pulse process set changed' }
$version = (Get-Item -LiteralPath "$root\Pulse.exe").VersionInfo.FileVersion
if ($version -ne '1.2.0.0') { throw "Unexpected version: $version" }
[pscustomobject]@{Version=$version;InstalledSmokeExitCode=$process.ExitCode;Screenshot=$shot;VerifiedPayloadFiles=$package.Files.Count;UnchangedPreferences=$preferences.Count;Service=$service;Processes=$processes;ManualAnimationAcceptance=$false;Time=(Get-Date).ToString('o')} |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath "$PSScriptRoot\installed-verification.json" -Encoding utf8
Get-Content -LiteralPath "$PSScriptRoot\installed-verification.json" -Raw
