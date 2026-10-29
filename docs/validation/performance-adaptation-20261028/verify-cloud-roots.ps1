$ErrorActionPreference = 'Stop'
$verifySource = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../../../build-release/pulse_cloud_features_test.exe'))
$verifyRoot = Join-Path ([IO.Path]::GetTempPath()) ('pulse-cloud-roots-' + [guid]::NewGuid().ToString('N'))
[IO.Directory]::CreateDirectory($verifyRoot) | Out-Null
$verifyProduct = Join-Path $verifyRoot 'pulse_cloud_features_test.exe'
Copy-Item -LiteralPath $verifySource -Destination $verifyProduct
$verifyHash = (Get-FileHash -LiteralPath $verifySource -Algorithm SHA256).Hash
if ((Get-FileHash -LiteralPath $verifyProduct -Algorithm SHA256).Hash -ne $verifyHash) { throw 'Test hash mismatch.' }
$verifySeeds = @()
$verifyAccounts = [Microsoft.Win32.Registry]::CurrentUser.OpenSubKey('Software\Microsoft\OneDrive\Accounts')
try {
    if (!$verifyAccounts) { throw 'No real OneDrive account registry.' }
    foreach ($verifyName in @('Personal') + @(1..16 | ForEach-Object { 'Business' + $_ })) {
        $verifyAccount = $verifyAccounts.OpenSubKey($verifyName)
        if (!$verifyAccount) { continue }
        try {
            $verifyPath = $verifyAccount.GetValue('UserFolder')
            if ($verifyPath) {
                if ($verifyPath.StartsWith('\\?\')) { $verifySeeds += $verifyPath }
                elseif ($verifyPath.StartsWith('\\')) { $verifySeeds += '\\?\UNC\' + $verifyPath.Substring(2) }
                else { $verifySeeds += '\\?\' + $verifyPath }
            }
        } finally { $verifyAccount.Dispose() }
    }
} finally { if ($verifyAccounts) { $verifyAccounts.Dispose() } }
if (!$verifySeeds.Count) { throw 'No OneDrive account folder seeds.' }
$verifyLog = Join-Path $PSScriptRoot 'cloud-roots-system.log'
& $verifyProduct --system-roots-only @verifySeeds *> $verifyLog
$verifyExit = $LASTEXITCODE
[ordered]@{ executable=$verifyProduct; sha256=$verifyHash; account_seed_count=$verifySeeds.Count;
    exit_code=$verifyExit; log=$verifyLog } | ConvertTo-Json |
    Set-Content -LiteralPath (Join-Path $PSScriptRoot 'cloud-roots-system-test.json') -Encoding UTF8
Get-Content -LiteralPath $verifyLog
exit $verifyExit
