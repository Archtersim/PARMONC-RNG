param(
    [string]$BaseDir = "C:\Users\tersi\Documents\Codex\2026-04-24\files-mentioned-by-the-user-poison",
    [int]$Threads = 16,
    [int]$Nr = 100,
    [int]$Tmod = 90,
    [double]$Coef = 1.0,
    [UInt64]$Block = 1000000,
    [int]$WarmupRuns = 1,
    [int]$MeasuredRuns = 3,
    [string]$OutDir = "C:\Users\tersi\Documents\Codex\2026-04-24\files-mentioned-by-the-user-poison\bench_compare_old_new_t16"
)

$ErrorActionPreference = "Stop"

function Assert-Exists([string]$PathToCheck) {
    if (-not (Test-Path $PathToCheck)) {
        throw "File not found: $PathToCheck"
    }
}

function To-Double([object]$x) {
    return [double](($x.ToString()) -replace ',', '.')
}

function Run-OldCore {
    param(
        [string]$Exe,
        [string]$Core,
        [int]$NrArg,
        [int]$TmodArg,
        [double]$CoefArg,
        [int]$ThreadsArg,
        [UInt64]$BlockArg
    )
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    & $Exe --rng-core $Core --Nr $NrArg --Tmod $TmodArg --coef $CoefArg --threads $ThreadsArg --block $BlockArg | Out-Null
    $sw.Stop()
    if ($LASTEXITCODE -ne 0) {
        throw "old run failed: core=$Core exit=$LASTEXITCODE"
    }
    return [double]$sw.Elapsed.TotalSeconds
}

function Read-CsvDouble([string]$v) {
    return [double](($v.ToString()) -replace ',', '.')
}

$oldExe = Join-Path $BaseDir "old_profiled_rngcore_compare.exe"
$newExeByCore = @{
    "v1" = (Join-Path $BaseDir "seird_v1_STRICT.exe")
    "v2" = (Join-Path $BaseDir "seird_v2_STRICT.exe")
    "v3" = (Join-Path $BaseDir "seird_v3_STRICT.exe")
    "v4" = (Join-Path $BaseDir "seird_v4_STRICT.exe")
}
$benchOpenmp = Join-Path $BaseDir "bench_openmp.ps1"

Assert-Exists $oldExe
Assert-Exists $benchOpenmp
foreach ($kv in $newExeByCore.GetEnumerator()) {
    Assert-Exists $kv.Value
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

# 1) old SEIRD + v1..v4 (threads parameter is passed through to old exe)
$oldRows = @()
$hostName = $env:COMPUTERNAME
$date = (Get-Date).ToString("yyyy-MM-ddTHH:mm:ss")
$cores = @("v1", "v2", "v3", "v4")

foreach ($core in $cores) {
    for ($w = 1; $w -le $WarmupRuns; $w++) {
        [void](Run-OldCore -Exe $oldExe -Core $core -NrArg $Nr -TmodArg $Tmod -CoefArg $Coef -ThreadsArg $Threads -BlockArg $Block)
    }
    for ($r = 1; $r -le $MeasuredRuns; $r++) {
        $sec = Run-OldCore -Exe $oldExe -Core $core -NrArg $Nr -TmodArg $Tmod -CoefArg $Coef -ThreadsArg $Threads -BlockArg $Block
        $oldRows += [PSCustomObject]@{
            family       = "old_seird"
            core         = $core
            run_id       = $r
            hostname     = $hostName
            date         = $date
            Nr           = $Nr
            Tmod         = $Tmod
            coef         = $Coef
            threads_arg  = $Threads
            block        = $Block
            time_total_s = [Math]::Round($sec, 6)
        }
    }
}

$oldCsv = Join-Path $OutDir "old_seird_v1_v2_v3_v4_t$Threads.csv"
$oldRows | Sort-Object core, run_id | Export-Csv -Path $oldCsv -NoTypeInformation -Encoding UTF8

# 2) new SEIRD + v1..v4 (real OpenMP threads)
$newCsvByCore = @{}
foreach ($core in $cores) {
    $exe = $newExeByCore[$core]
    $csv = Join-Path $OutDir ("new_seird_{0}_t{1}.csv" -f $core, $Threads)
    $newCsvByCore[$core] = $csv
    powershell -ExecutionPolicy Bypass -File $benchOpenmp `
        -Exe $exe `
        -Nr $Nr `
        -Tmod $Tmod `
        -Block $Block `
        -ThreadsList "1,$Threads" `
        -WarmupRuns $WarmupRuns `
        -MeasuredRuns $MeasuredRuns `
        -CsvPath $csv | Out-Null

    if (-not (Test-Path $csv)) {
        throw "Expected CSV was not created: $csv"
    }
}

# 3) unified runs CSV
$all = @()
$all += ($oldRows | ForEach-Object {
    [PSCustomObject]@{
        family       = $_.family
        core         = $_.core
        run_id       = $_.run_id
        threads      = $_.threads_arg
        time_total_s = [double]$_.time_total_s
    }
})

foreach ($core in $cores) {
    $rows = Import-Csv $newCsvByCore[$core]
    foreach ($r in $rows) {
        $all += [PSCustomObject]@{
            family       = "new_seird"
            core         = $core
            run_id       = [int]$r.run_id
            threads      = [int]$r.threads
            time_total_s = Read-CsvDouble $r.time_total_s
        }
    }
}

$runsCsv = Join-Path $OutDir "old_new_seird_v1_v2_v3_v4_t$Threads_runs.csv"
$all | Sort-Object core, family, run_id | Export-Csv -Path $runsCsv -NoTypeInformation -Encoding UTF8

# 4) summary + old->new speedup
$summary = $all |
    Group-Object family, core |
    ForEach-Object {
        $g = $_.Group
        [PSCustomObject]@{
            family = $g[0].family
            core = $g[0].core
            threads = $g[0].threads
            avg_s = [Math]::Round(($g | Measure-Object time_total_s -Average).Average, 6)
            min_s = [Math]::Round(($g | Measure-Object time_total_s -Minimum).Minimum, 6)
            max_s = [Math]::Round(($g | Measure-Object time_total_s -Maximum).Maximum, 6)
        }
    } | Sort-Object core, family

$speedRows = @()
foreach ($core in $cores) {
    $oldAvg = ($summary | Where-Object { $_.core -eq $core -and $_.family -eq "old_seird" }).avg_s
    $newAvg = ($summary | Where-Object { $_.core -eq $core -and $_.family -eq "new_seird" }).avg_s
    $speedRows += [PSCustomObject]@{
        core = $core
        old_avg_s = $oldAvg
        new_avg_s = $newAvg
        speedup_old_to_new = [Math]::Round(($oldAvg / $newAvg), 6)
    }
}

$summaryCsv = Join-Path $OutDir "summary_old_new_seird_v1_v2_v3_v4_t$Threads.csv"
$summary | Export-Csv -Path $summaryCsv -NoTypeInformation -Encoding UTF8

$speedCsv = Join-Path $OutDir "speedup_old_to_new_v1_v2_v3_v4_t$Threads.csv"
$speedRows | Export-Csv -Path $speedCsv -NoTypeInformation -Encoding UTF8

"Done."
"OutDir: $OutDir"
"Runs: $runsCsv"
"Summary: $summaryCsv"
"Speedup: $speedCsv"
$speedRows | Format-Table -AutoSize
