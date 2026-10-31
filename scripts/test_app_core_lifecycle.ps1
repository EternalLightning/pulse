# SPDX-License-Identifier: Apache-2.0
# Pulse project; see LICENSE and NOTICE for licensing and attribution.
# Modified by EternalLightning for this independent distribution, including licensing notices.
# Existing third-party notices and license terms remain applicable.

# Run from an x64 Visual Studio developer prompt. The fixture links the real
# application objects, so SkipBuild requires a fresh, matching production build.
param(
    [Parameter(Mandatory = $true)][string]$BuildDir,
    [switch]$SkipBuild,
    [switch]$ShotTimingOnly,
    [ValidateRange(10, 180)][int]$TimeoutSeconds = 60
)
$ErrorActionPreference = 'Stop'
$taskRoot = Split-Path $PSScriptRoot -Parent
Push-Location $taskRoot
try {
    $taskBuild = (Resolve-Path -LiteralPath $BuildDir).Path
    if (!$SkipBuild) {
        cmake --build $taskBuild --target pulse
        if ($LASTEXITCODE) { throw 'Pulse build failed.' }
    }
    $taskLines = Get-Content (Join-Path $taskBuild 'build.ninja')
    $taskCompile = ($taskLines | Select-String '^build CMakeFiles[\\/]pulse.dir[\\/]src[\\/]app[\\/]address_search.cpp.obj:' | Select-Object -First 1).LineNumber
    $taskLink = ($taskLines | Select-String '^build Pulse.exe:' | Select-Object -First 1).LineNumber
    if (!$taskCompile -or !$taskLink) { throw 'Expected the single-configuration Ninja build.' }
    $taskCompileArgs = ($taskLines[$taskCompile..($taskCompile + 5)] | Where-Object { $_ -match '^  (DEFINES|FLAGS|INCLUDES) = ' }) -replace '^  \w+ = ', ''
    $taskCompileArgs += "/c src/bench/app_core_lifecycle_test.cpp /Fo`"$taskBuild/app_core_lifecycle_test.obj`" /Fd`"$taskBuild/app_core_lifecycle_test_compile.pdb`""
    $taskCompileResponse = Join-Path $taskBuild 'app_core_lifecycle_compile.rsp'
    $taskCompileArgs | Set-Content $taskCompileResponse -Encoding unicode
    & cl.exe /nologo "@$taskCompileResponse"
    if ($LASTEXITCODE) { throw 'App core lifecycle fixture compilation failed.' }
    $taskObjects = ($taskLines[$taskLink - 1] -replace '^build Pulse.exe: \S+ ', '') -split ' \|' | Select-Object -First 1
    $taskLibraries = ($taskLines[$taskLink..($taskLink + 8)] | Where-Object { $_ -match '^  LINK_LIBRARIES = ' }) -replace '^  LINK_LIBRARIES = ', ''
    $taskDelayLoad = if (($taskLibraries -join ' ') -match '(?i)(?:^|[\\/\s"])lumatext\.lib(?:$|[\s"])') { '/DELAYLOAD:lumatext.dll' } else { '' }
    @($taskObjects, $taskLibraries, "app_core_lifecycle_test.obj /out:pulse_app_core_lifecycle_test.exe /pdb:app_core_lifecycle_test.pdb /subsystem:console /machine:x64 /INCREMENTAL:NO /MANIFEST:NO /LTCG /guard:cf /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT $taskDelayLoad") | Set-Content (Join-Path $taskBuild 'app_core_lifecycle_link.rsp') -Encoding unicode
    Push-Location $taskBuild
    try {
        & link.exe /nologo '@app_core_lifecycle_link.rsp'
        if ($LASTEXITCODE) { throw 'App core lifecycle fixture linking failed.' }
    } finally { Pop-Location }
    $taskOutput = Join-Path $taskRoot 'bench_data/audit-fixes-20261030'
    [System.IO.Directory]::CreateDirectory($taskOutput) | Out-Null
    $taskStdout = Join-Path $taskOutput 'app-core-test.stdout.log'
    $taskStderr = Join-Path $taskOutput 'app-core-test.stderr.log'
    $taskStart = New-Object System.Diagnostics.ProcessStartInfo
    $taskStart.FileName = Join-Path $taskBuild 'pulse_app_core_lifecycle_test.exe'
    $taskStart.WorkingDirectory = $taskRoot
    if ($ShotTimingOnly) { $taskStart.Arguments = '--shot-timing' }
    $taskStart.UseShellExecute = $false
    $taskStart.CreateNoWindow = $true
    $taskStart.RedirectStandardOutput = $true
    $taskStart.RedirectStandardError = $true
    $taskProcess = New-Object System.Diagnostics.Process
    $taskProcess.StartInfo = $taskStart
    try {
        if (!$taskProcess.Start()) { throw 'App core lifecycle fixture did not start.' }
        $taskReadOutput = $taskProcess.StandardOutput.ReadToEndAsync()
        $taskReadError = $taskProcess.StandardError.ReadToEndAsync()
        $taskTimedOut = !$taskProcess.WaitForExit($TimeoutSeconds * 1000)
        if ($taskTimedOut) { $taskProcess.Kill(); $taskProcess.WaitForExit() }
        $taskOutputText = $taskReadOutput.GetAwaiter().GetResult()
        $taskErrorText = $taskReadError.GetAwaiter().GetResult()
        [System.IO.File]::WriteAllText($taskStdout, $taskOutputText)
        [System.IO.File]::WriteAllText($taskStderr, $taskErrorText)
        Write-Output $taskOutputText
        if ($taskErrorText) { Write-Output $taskErrorText }
        if ($taskTimedOut) { throw "App core lifecycle fixture exceeded $TimeoutSeconds seconds." }
        if ($null -eq $taskProcess.ExitCode -or $taskProcess.ExitCode -ne 0) {
            throw "App core lifecycle fixture failed: $($taskProcess.ExitCode)."
        }
    } finally { $taskProcess.Dispose() }
} finally { Pop-Location }
