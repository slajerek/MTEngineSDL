<#
.SYNOPSIS
    Exercise the Windows driver's actual FFmpeg gate without building an app.
.DESCRIPTION
    Gate cases execute the unchanged source between the driver's existing
    licence-gate and post-build-hook comments, with recording scanner doubles.
    Separate scanner cases run the real scripts with controlled dumpbin output
    and small byte fixtures, not real PE/codec validation.  Each case uses a
    fresh PowerShell process so product exit statements cannot stop the suite.

    Run: powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File tests\test-windows-ffmpeg-gate.ps1
    No Pester, Visual Studio, FFmpeg, downloads, or application build is needed.
    Logs are retained in ResultsDir (a new temporary directory by default).
.PARAMETER DriverPath
    Optional original driver path for a before-fix regression check.
#>
param(
    [string]$DriverPath = (Join-Path $PSScriptRoot '..\tools\appbuild\app-build-windows.ps1'),
    [string]$ResultsDir,
    # Internal child-process arguments.
    [string]$CaseFile,
    [string]$GateFile
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

function Test-DiagnosticText([string]$Text, [string]$Expected) {
    # PowerShell's error formatter wraps at spaces according to host width.
    # Compare the same words, but retain raw text for success-sentinel checks.
    return ([regex]::Replace($Text, '\s+', ' ').Trim()).Contains(
        [regex]::Replace($Expected, '\s+', ' ').Trim())
}

if ($CaseFile) {
    $case = Get-Content -LiteralPath $CaseFile -Raw | ConvertFrom-Json
    $work = Split-Path -Parent $CaseFile
    $mtLibsDir = Join-Path $work 'libs'
    $outDir = Join-Path $work 'out'
    New-Item -ItemType Directory -Path $mtLibsDir, $outDir | Out-Null
    $exe = Join-Path $outDir 'fixture.exe'
    [IO.File]::WriteAllText($exe, 'fixture, not a PE')
    if ($null -eq $case.Env) { Remove-Item Env:MT_ENABLE_FFMPEG -ErrorAction SilentlyContinue }
    else { $env:MT_ENABLE_FFMPEG = $case.Env }

    if ($case.Kind -eq 'gate') {
        $mtDir = Join-Path $work 'engine'
        $scanners = Join-Path $mtDir 'tools\appbuild'
        New-Item -ItemType Directory -Path $scanners -Force | Out-Null
        $env:FFMPEG_TEST_TRACE = Join-Path $work 'calls.jsonl'
        $env:FFMPEG_TEST_FORBIDDEN_EXIT = [string]$case.ForbiddenExit
        $env:FFMPEG_TEST_PRESENCE_EXIT = [string]$case.PresenceExit
        @'
param([string]$Mode, [string]$Libs)
[ordered]@{scanner='forbidden'; mode=$Mode; libs=$Libs} | ConvertTo-Json -Compress | Add-Content -LiteralPath $env:FFMPEG_TEST_TRACE
exit ([int]$env:FFMPEG_TEST_FORBIDDEN_EXIT)
'@ | Set-Content -LiteralPath (Join-Path $scanners 'scan-forbidden-symbols.ps1') -Encoding UTF8
        @'
param([string]$Binary, [string]$Dir, [string]$Expect)
[ordered]@{scanner='presence'; binary=$Binary; dir=$Dir; expect=$Expect} | ConvertTo-Json -Compress | Add-Content -LiteralPath $env:FFMPEG_TEST_TRACE
exit ([int]$env:FFMPEG_TEST_PRESENCE_EXIT)
'@ | Set-Content -LiteralPath (Join-Path $scanners 'scan-no-ffmpeg.ps1') -Encoding UTF8
        if ($null -ne $case.Marker) {
            $prefix = Join-Path $mtLibsDir 'ffmpeg'
            New-Item -ItemType Directory -Path $prefix | Out-Null
            Set-Content -LiteralPath (Join-Path $prefix '.ffmpeg-build-mode') -Value $case.Marker
        }
        $mtResolvedFlags = @($case.Flags)
        $commercial = $case.Commercial
        $ffmpegMode = $case.Mode
        # This is the driver's actual source, including calls and refusals.
        & ([scriptblock]::Create((Get-Content -LiteralPath $GateFile -Raw)))
        Write-Output 'GATE_COMPLETED'
    } elseif ($case.Kind -eq 'presence') {
        # The scanner is a separate script scope; share only within this
        # disposable child process, not through the harness's script scope.
        $global:ffmpegFixtureImports = $case.Imports
        function dumpbin {
            $global:LASTEXITCODE = 0
            $global:ffmpegFixtureImports
        }
        if ($case.Staged) {
            $nested = Join-Path $outDir 'nested'
            New-Item -ItemType Directory -Path $nested | Out-Null
            [IO.File]::WriteAllText((Join-Path $nested 'avcodec-61.dll'), 'staged fixture')
        }
        & (Join-Path $root 'tools\appbuild\scan-no-ffmpeg.ps1') -Binary $exe -Dir $outDir -Expect $case.Expect
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        Write-Output 'SCANNER_COMPLETED'
    } elseif ($case.Kind -eq 'forbidden') {
        if ($null -ne $case.Codec) {
            [IO.File]::WriteAllText((Join-Path $mtLibsDir 'avcodec-fixture.dll'), $case.Codec, [Text.Encoding]::ASCII)
        }
        & (Join-Path $root 'tools\appbuild\scan-forbidden-symbols.ps1') -Mode $case.Mode -Libs $mtLibsDir
        if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
        Write-Output 'SCANNER_COMPLETED'
    } else { throw "Unknown case kind: $($case.Kind)" }
    exit 0
}

# Matcher controls are independent of driver/scanner behavior and case outcomes.
if (-not (Test-DiagnosticText "read no import `r`ntable" 'read no import table') -or
    -not (Test-DiagnosticText "no --enable-decoder clause `r`nfound" 'no --enable-decoder clause found') -or
    (Test-DiagnosticText 'read import table' 'read no import table') -or
    (Test-DiagnosticText 'read no export table' 'read no import table') -or
    (Test-DiagnosticText '' 'read no import table')) {
    throw 'Diagnostic matcher controls failed'
}

# Extract only from genuine comment tokens, and refuse drift or syntax errors.
$tokens = $null
$parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile(
    (Resolve-Path -LiteralPath $DriverPath).Path, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -ne 0) { throw "Driver parse errors: $parseErrors" }
$start = @($tokens | Where-Object { $_.Kind -eq 'Comment' -and $_.Text -match '^# --- licence gate -+$' })
$end = @($tokens | Where-Object { $_.Kind -eq 'Comment' -and $_.Text -match '^# --- app post-build hook \(optional\) -+$' })
if ($start.Count -ne 1 -or $end.Count -ne 1 -or $end[0].Extent.StartOffset -le $start[0].Extent.EndOffset) {
    throw 'Expected exactly one ordered licence-gate/post-build-hook boundary pair'
}
$source = $ast.Extent.Text
$gate = $source.Substring($start[0].Extent.EndOffset, $end[0].Extent.StartOffset - $start[0].Extent.EndOffset)
$gateAst = [System.Management.Automation.Language.Parser]::ParseInput($gate, [ref]$tokens, [ref]$parseErrors)
if ($parseErrors.Count -ne 0 -or $gateAst.EndBlock.Statements.Count -eq 0) { throw 'Empty or invalid gate source' }
if (-not $ResultsDir) { $ResultsDir = Join-Path ([IO.Path]::GetTempPath()) ('mt-ffmpeg-gate-' + [guid]::NewGuid().ToString('N')) }
if (Test-Path -LiteralPath $ResultsDir) { throw "ResultsDir must be new: $ResultsDir" }
New-Item -ItemType Directory -Path $ResultsDir | Out-Null
$ResultsDir = (Resolve-Path -LiteralPath $ResultsDir).Path
$GateFile = Join-Path $ResultsDir 'gate.ps1'
[IO.File]::WriteAllText($GateFile, $gate)
Write-Host "Results: $ResultsDir"

$cases = New-Object 'System.Collections.Generic.List[object]'
function Add-Case([string]$name, [hashtable]$changes) {
    $c = @{
        Name=$name; Kind='gate'; Flags=@('MT_ENABLE_FFMPEG = 0'); Env=$null
        Commercial='1'; Mode='commercial'; Marker=$null; ForbiddenExit=0; PresenceExit=0
        Calls=@('presence:absent'); ErrorText=$null; WarningText=$null
        Imports='KERNEL32.dll'; Staged=$false; Expect='absent'; Codec=$null
    }
    foreach ($key in $changes.Keys) { $c[$key] = $changes[$key] }
    $cases.Add($c)
}
foreach ($ambient in @($null, '0', '1')) {
    $label = if ($null -eq $ambient) { 'unset' } else { $ambient }
    Add-Case "disabled, environment $label, no dependency-stage import" @{Env=$ambient}
    Add-Case "enabled, environment $label" @{
        Flags=@('MT_ENABLE_FFMPEG = 1'); Env=$ambient; Marker='commercial'
        Calls=@('forbidden:commercial', 'presence:present')
    }
}
Add-Case 'unrelated flags do not change selection' @{
    Flags=@('# MT_ENABLE_FFMPEG = 1', 'MT_ENABLE_FFMPEG_EXTRA = 1', 'MT_ENABLE_FFMPEG = 0', 'MT_DEBUG_LOGS = 1')
}
Add-Case 'full mode retains presence scan' @{
    Flags=@('MT_ENABLE_FFMPEG = 1'); Commercial='0'; Mode='full'; Marker='full'; Calls=@('presence:present')
}
foreach ($marker in @($null, 'full')) {
    Add-Case "commercial marker refusal: $marker" @{
        Flags=@('MT_ENABLE_FFMPEG = 1'); Marker=$marker; Calls=@(); ErrorText='commercial app build against'
    }
}
Add-Case 'noncommercial stale marker warns and scans' @{
    Flags=@('MT_ENABLE_FFMPEG = 1'); Commercial='0'; Marker='full'
    Calls=@('forbidden:commercial', 'presence:present'); WarningText='stale prefix'
}
Add-Case 'forbidden scan failure stops the gate' @{
    Flags=@('MT_ENABLE_FFMPEG = 1'); Marker='commercial'; ForbiddenExit=7
    Calls=@('forbidden:commercial'); ErrorText='forbidden-decoder scan failed'
}
Add-Case 'absence scan failure stops the gate' @{PresenceExit=7; ErrorText='no-FFmpeg scan failed'}
Add-Case 'presence scan failure stops the gate' @{
    Flags=@('MT_ENABLE_FFMPEG = 1'); Marker='commercial'; PresenceExit=7
    Calls=@('forbidden:commercial', 'presence:present'); ErrorText='no-FFmpeg scan failed'
}
$invalid = @(
    @{Name='missing'; Flags=@('MT_DEBUG_LOGS = 1')},
    @{Name='empty'; Flags=@('MT_ENABLE_FFMPEG = ')},
    @{Name='two'; Flags=@('MT_ENABLE_FFMPEG = 2')},
    @{Name='boolean'; Flags=@('MT_ENABLE_FFMPEG = true')},
    @{Name='no assignment'; Flags=@('MT_ENABLE_FFMPEG')},
    @{Name='duplicate zero'; Flags=@('MT_ENABLE_FFMPEG = 0', 'MT_ENABLE_FFMPEG = 0')},
    @{Name='duplicate one'; Flags=@('MT_ENABLE_FFMPEG = 1', 'MT_ENABLE_FFMPEG = 1')},
    @{Name='conflicting'; Flags=@('MT_ENABLE_FFMPEG = 0', 'MT_ENABLE_FFMPEG = 1')},
    @{Name='valid plus malformed'; Flags=@('MT_ENABLE_FFMPEG = 0', 'MT_ENABLE_FFMPEG = true')}
)
foreach ($bad in $invalid) {
    Add-Case "invalid resolved flag: $($bad.Name)" @{
        Flags=$bad.Flags; Calls=@(); ErrorText='Expected exactly one MT_ENABLE_FFMPEG = 0 or 1'
    }
}
Add-Case 'real scanner: clean absence' @{Kind='presence'; Calls=@()}
Add-Case 'real scanner: imported FFmpeg refused' @{
    Kind='presence'; Calls=@(); Imports='avcodec-61.dll'; ErrorText='declared MT_ENABLE_FFMPEG=0 but carries FFmpeg'
}
Add-Case 'real scanner: nested staged FFmpeg refused' @{
    Kind='presence'; Calls=@(); Staged=$true; ErrorText='declared MT_ENABLE_FFMPEG=0 but carries FFmpeg'
}
Add-Case 'real scanner: unprovable imports refused' @{
    Kind='presence'; Calls=@(); Imports=''; ErrorText='read no import table'
}
Add-Case 'real scanner: required FFmpeg missing' @{
    Kind='presence'; Calls=@(); Expect='present'; ErrorText='expected FFmpeg to be present and found none'
}
Add-Case 'real scanner: required FFmpeg present' @{
    Kind='presence'; Calls=@(); Expect='present'; Imports='avcodec-61.dll'
}
Add-Case 'real decoder scanner: allowed fixture' @{Kind='forbidden'; Calls=@(); Codec='--enable-decoder=h264'}
Add-Case 'real decoder scanner: forbidden fixture' @{
    Kind='forbidden'; Calls=@(); Codec='--enable-decoder=hevc'; ErrorText='patent-encumbered decoders'
}
Add-Case 'real decoder scanner: missing DLL' @{Kind='forbidden'; Calls=@(); ErrorText='no avcodec DLL found'}
Add-Case 'real decoder scanner: unprovable configuration' @{
    Kind='forbidden'; Calls=@(); Codec='no configure clause'; ErrorText='no --enable-decoder clause found'
}

$hostExe = (Get-Process -Id $PID).Path
$results = @()
$index = 0
foreach ($case in $cases) {
    $index++
    $work = Join-Path $ResultsDir ('{0:D2}' -f $index)
    New-Item -ItemType Directory -Path $work | Out-Null
    $CaseFile = Join-Path $work 'case.json'
    $case | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $CaseFile -Encoding UTF8
    $stdout = Join-Path $work 'stdout.txt'
    $stderr = Join-Path $work 'stderr.txt'
    $childArgs = @('-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File',
        ('"{0}"' -f $PSCommandPath), '-CaseFile', ('"{0}"' -f $CaseFile), '-GateFile', ('"{0}"' -f $GateFile))
    $process = Start-Process -FilePath $hostExe -ArgumentList $childArgs -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $null = $process.Handle
    $timedOut = -not $process.WaitForExit(30000)
    if ($timedOut) {
        $process.Kill()
        if (-not $process.WaitForExit(5000)) { throw "Child did not terminate: $($case.Name)" }
    }
    $process.Refresh()
    $code = $process.ExitCode
    [string]$text = (Get-Content -LiteralPath $stdout -Raw) + (Get-Content -LiteralPath $stderr -Raw)
    $sentinel = if ($case.Kind -eq 'gate') { 'GATE_COMPLETED' } else { 'SCANNER_COMPLETED' }
    $ok = -not $timedOut -and $null -ne $code
    if ($case.ErrorText) {
        $ok = $ok -and $code -ne 0 -and (Test-DiagnosticText $text $case.ErrorText) -and -not $text.Contains($sentinel)
    } else {
        $ok = $ok -and $code -eq 0 -and $text.Contains($sentinel)
    }
    if ($case.WarningText) { $ok = $ok -and (Test-DiagnosticText $text $case.WarningText) }
    $actual = @()
    $trace = Join-Path $work 'calls.jsonl'
    if (Test-Path -LiteralPath $trace) { $actual = @(Get-Content -LiteralPath $trace | ForEach-Object { $_ | ConvertFrom-Json }) }
    $ok = $ok -and $actual.Count -eq $case.Calls.Count
    for ($i = 0; $i -lt $actual.Count; $i++) {
        $call = $actual[$i]
        if ($call.scanner -eq 'forbidden') {
            $ok = $ok -and $i -lt $case.Calls.Count -and $case.Calls[$i] -eq "forbidden:$($call.mode)" -and $call.libs -eq (Join-Path $work 'libs')
        } elseif ($call.scanner -eq 'presence') {
            $ok = $ok -and $i -lt $case.Calls.Count -and $case.Calls[$i] -eq "presence:$($call.expect)" -and $call.binary -eq (Join-Path $work 'out\fixture.exe') -and $call.dir -eq (Join-Path $work 'out')
        } else { $ok = $false }
    }
    $results += [pscustomobject]@{name=$case.Name; pass=[bool]$ok; exitCode=$code; timedOut=$timedOut; directory=$work}
    Write-Host "$(if ($ok) { 'PASS' } else { 'FAIL' }): $($case.Name)"
    $process.Dispose()
}
$results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $ResultsDir 'results.json') -Encoding UTF8
$failed = @($results | Where-Object { -not $_.pass }).Count
Write-Host "RESULT: $($results.Count - $failed)/$($results.Count) passed"
if ($failed) { exit 1 }
exit 0
