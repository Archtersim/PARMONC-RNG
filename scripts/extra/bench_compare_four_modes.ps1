param(
    [string]$BaseDir = "C:\Users\tersi\Documents\Codex\2026-04-24\files-mentioned-by-the-user-poison",
    [string]$Gpp = "C:\msys64\ucrt64\bin\g++.exe",
    [string]$CoresList = "v1,v2,v3,v4",
    [int]$Nr = 100,
    [int]$Tmod = 90,
    [UInt64]$Block = 1000000,
    [int]$ModelThreads = 16,
    [int]$RngThreads = 16,
    [UInt64]$RngPrefetch = 65536,
    [int]$WarmupRuns = 1,
    [int]$MeasuredRuns = 3,
    [switch]$SkipBuild,
    [switch]$SkipOld,
    [string]$OutDir = ""
)

$ErrorActionPreference = "Stop"

if (-not $OutDir) {
    $OutDir = Join-Path $BaseDir "bench_compare_four_modes"
}
if (-not (Test-Path $OutDir)) {
    New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
}

function Log([string]$msg) {
    $ts = (Get-Date).ToString("yyyy-MM-dd HH:mm:ss")
    Write-Host "[$ts] $msg"
}

function Assert-Exists([string]$path) {
    if (-not (Test-Path $path)) {
        throw "File not found: $path"
    }
}

function D([object]$x) {
    return [double](($x.ToString()) -replace ',', '.')
}

$buildV1 = Join-Path $BaseDir "build_seird_strict_v1.ps1"
$buildV34 = Join-Path $BaseDir "build_seird_strict_v34.ps1"
$buildModelopt = Join-Path $BaseDir "build_seird_modelopt_v1_v4.ps1"
$benchOpenmp = Join-Path $BaseDir "bench_openmp.ps1"
$benchOld = Join-Path $BaseDir "bench_old_arch_rngcore.ps1"
$oldExe = Join-Path $BaseDir "old_profiled_rngcore_compare.exe"

$requiredPaths = @($buildV1, $buildV34, $buildModelopt, $benchOpenmp)
if (-not $SkipOld) {
    $requiredPaths += @($benchOld, $oldExe)
}

foreach ($p in $requiredPaths) {
    Assert-Exists $p
}

if (-not $SkipBuild) {
    Log "Building strict SEIRD binaries..."
    powershell -ExecutionPolicy Bypass -File $buildV1 -Gpp $Gpp
    powershell -ExecutionPolicy Bypass -File $buildV34 -Gpp $Gpp
    powershell -ExecutionPolicy Bypass -File $buildModelopt -Gpp $Gpp
}

$exeByCore = @{
    "v1" = (Join-Path $BaseDir "seird_v1_STRICT.exe")
    "v2" = (Join-Path $BaseDir "seird_v2_STRICT.exe")
    "v3" = (Join-Path $BaseDir "seird_v3_STRICT.exe")
    "v4" = (Join-Path $BaseDir "seird_v4_STRICT.exe")
}
foreach ($kv in $exeByCore.GetEnumerator()) {
    Assert-Exists $kv.Value
}

$modeloptExeByCore = @{
    "v1" = (Join-Path $BaseDir "seird_modelopt_v1.exe")
    "v2" = (Join-Path $BaseDir "seird_modelopt_v2.exe")
    "v3" = (Join-Path $BaseDir "seird_modelopt_v3.exe")
    "v4" = (Join-Path $BaseDir "seird_modelopt_v4.exe")
}
foreach ($kv in $modeloptExeByCore.GetEnumerator()) {
    Assert-Exists $kv.Value
}

$cores = $CoresList.Split(",") | ForEach-Object { $_.Trim() } | Where-Object { $_ -match '^v[1-4]$' }
if ($cores.Count -eq 0) {
    throw "CoresList is empty. Example: -CoresList 'v2,v3,v4'"
}
$allRows = @()

if (-not $SkipOld) {
    Log "Scenario 1/6: old_seird_1t"
    $oldCsv = Join-Path $OutDir "old_seird_1t_runs.csv"
    powershell -ExecutionPolicy Bypass -File $benchOld `
        -Exe $oldExe `
        -Cores ($cores -join ",") `
        -Nr $Nr `
        -Tmod $Tmod `
        -Coef 1 `
        -WarmupRuns $WarmupRuns `
        -MeasuredRuns $MeasuredRuns `
        -CsvPath $oldCsv

    $oldRows = Import-Csv $oldCsv
    foreach ($r in $oldRows) {
        $allRows += [pscustomobject]@{
            scenario     = "old_seird_1t"
            core         = $r.core
            run_id       = [int]$r.run_id
            model_threads = 1
            rng_threads  = 1
            rng_prefetch = 0
            avg_input_Nr = $Nr
            avg_input_Tmod = $Tmod
            avg_input_block = $Block
            time_total_s = D $r.time_total_s
        }
    }
}

Log "Scenario 2/4: new_seird_1t"
foreach ($core in $cores) {
    $csv = Join-Path $OutDir ("new_seird_1t_{0}.csv" -f $core)
    powershell -ExecutionPolicy Bypass -File $benchOpenmp `
        -Exe $exeByCore[$core] `
        -Nr $Nr `
        -Tmod $Tmod `
        -Block $Block `
        -ThreadsList "1" `
        -RngThreads 1 `
        -RngPrefetch $RngPrefetch `
        -WarmupRuns $WarmupRuns `
        -MeasuredRuns $MeasuredRuns `
        -CsvPath $csv

    foreach ($r in (Import-Csv $csv)) {
        $allRows += [pscustomobject]@{
            scenario     = "new_seird_1t"
            core         = $core
            run_id       = [int]$r.run_id
            model_threads = [int]$r.threads
            rng_threads  = [int]$r.rng_threads
            rng_prefetch = [UInt64]$r.rng_prefetch
            avg_input_Nr = $Nr
            avg_input_Tmod = $Tmod
            avg_input_block = $Block
            time_total_s = D $r.time_total_s
        }
    }
}

Log "Scenario 3/4: new_seird_${ModelThreads}t"
foreach ($core in $cores) {
    $csv = Join-Path $OutDir ("new_seird_{0}t_{1}.csv" -f $ModelThreads, $core)
    powershell -ExecutionPolicy Bypass -File $benchOpenmp `
        -Exe $exeByCore[$core] `
        -Nr $Nr `
        -Tmod $Tmod `
        -Block $Block `
        -ThreadsList ("1,{0}" -f $ModelThreads) `
        -RngThreads 1 `
        -RngPrefetch $RngPrefetch `
        -WarmupRuns $WarmupRuns `
        -MeasuredRuns $MeasuredRuns `
        -CsvPath $csv

    foreach ($r in (Import-Csv $csv | Where-Object { [int]$_.threads -eq $ModelThreads })) {
        $allRows += [pscustomobject]@{
            scenario     = ("new_seird_{0}t" -f $ModelThreads)
            core         = $core
            run_id       = [int]$r.run_id
            model_threads = [int]$r.threads
            rng_threads  = [int]$r.rng_threads
            rng_prefetch = [UInt64]$r.rng_prefetch
            avg_input_Nr = $Nr
            avg_input_Tmod = $Tmod
            avg_input_block = $Block
            time_total_s = D $r.time_total_s
        }
    }
}

Log "Scenario 4/4: model1_rng${RngThreads}"
foreach ($core in $cores) {
    $csv = Join-Path $OutDir ("model1_rng{0}_{1}.csv" -f $RngThreads, $core)
    powershell -ExecutionPolicy Bypass -File $benchOpenmp `
        -Exe $exeByCore[$core] `
        -Nr $Nr `
        -Tmod $Tmod `
        -Block $Block `
        -ThreadsList "1" `
        -RngThreads $RngThreads `
        -RngPrefetch $RngPrefetch `
        -WarmupRuns $WarmupRuns `
        -MeasuredRuns $MeasuredRuns `
        -CsvPath $csv

    foreach ($r in (Import-Csv $csv | Where-Object { [int]$_.threads -eq 1 })) {
        $allRows += [pscustomobject]@{
            scenario     = ("model1_rng{0}" -f $RngThreads)
            core         = $core
            run_id       = [int]$r.run_id
            model_threads = [int]$r.threads
            rng_threads  = [int]$r.rng_threads
            rng_prefetch = [UInt64]$r.rng_prefetch
            avg_input_Nr = $Nr
            avg_input_Tmod = $Tmod
            avg_input_block = $Block
            time_total_s = D $r.time_total_s
        }
    }
}

Log "Scenario 5/6: modelopt_1t"
foreach ($core in $cores) {
    $csv = Join-Path $OutDir ("modelopt_1t_{0}.csv" -f $core)
    powershell -ExecutionPolicy Bypass -File $benchOpenmp `
        -Exe $modeloptExeByCore[$core] `
        -Nr $Nr `
        -Tmod $Tmod `
        -Block $Block `
        -ThreadsList "1" `
        -RngThreads 1 `
        -RngPrefetch $RngPrefetch `
        -WarmupRuns $WarmupRuns `
        -MeasuredRuns $MeasuredRuns `
        -CsvPath $csv

    foreach ($r in (Import-Csv $csv)) {
        $allRows += [pscustomobject]@{
            scenario      = "modelopt_1t"
            core          = $core
            run_id        = [int]$r.run_id
            model_threads = [int]$r.threads
            rng_threads   = [int]$r.rng_threads
            rng_prefetch  = [UInt64]$r.rng_prefetch
            avg_input_Nr = $Nr
            avg_input_Tmod = $Tmod
            avg_input_block = $Block
            time_total_s  = D $r.time_total_s
        }
    }
}

Log "Scenario 6/6: modelopt_${ModelThreads}t"
foreach ($core in $cores) {
    $csv = Join-Path $OutDir ("modelopt_{0}t_{1}.csv" -f $ModelThreads, $core)
    powershell -ExecutionPolicy Bypass -File $benchOpenmp `
        -Exe $modeloptExeByCore[$core] `
        -Nr $Nr `
        -Tmod $Tmod `
        -Block $Block `
        -ThreadsList ("1,{0}" -f $ModelThreads) `
        -RngThreads 1 `
        -RngPrefetch $RngPrefetch `
        -WarmupRuns $WarmupRuns `
        -MeasuredRuns $MeasuredRuns `
        -CsvPath $csv

    foreach ($r in (Import-Csv $csv | Where-Object { [int]$_.threads -eq $ModelThreads })) {
        $allRows += [pscustomobject]@{
            scenario      = ("modelopt_{0}t" -f $ModelThreads)
            core          = $core
            run_id        = [int]$r.run_id
            model_threads = [int]$r.threads
            rng_threads   = [int]$r.rng_threads
            rng_prefetch  = [UInt64]$r.rng_prefetch
            avg_input_Nr = $Nr
            avg_input_Tmod = $Tmod
            avg_input_block = $Block
            time_total_s  = D $r.time_total_s
        }
    }
}

$runsCsv = Join-Path $OutDir "four_modes_runs.csv"
$allRows | Sort-Object core, scenario, run_id | Export-Csv -Path $runsCsv -NoTypeInformation -Encoding UTF8

$summary = $allRows |
    Group-Object scenario, core |
    ForEach-Object {
        $g = $_.Group
        [pscustomobject]@{
            scenario      = $g[0].scenario
            core          = $g[0].core
            model_threads = $g[0].model_threads
            rng_threads   = $g[0].rng_threads
            rng_prefetch  = $g[0].rng_prefetch
            avg_s         = [Math]::Round(($g | Measure-Object time_total_s -Average).Average, 6)
            min_s         = [Math]::Round(($g | Measure-Object time_total_s -Minimum).Minimum, 6)
            max_s         = [Math]::Round(($g | Measure-Object time_total_s -Maximum).Maximum, 6)
        }
    } | Sort-Object core, scenario

$summaryCsv = Join-Path $OutDir "four_modes_summary.csv"
$summary | Export-Csv -Path $summaryCsv -NoTypeInformation -Encoding UTF8

Log "Done."
Log "Runs: $runsCsv"
Log "Summary: $summaryCsv"
$summary | Format-Table -AutoSize
