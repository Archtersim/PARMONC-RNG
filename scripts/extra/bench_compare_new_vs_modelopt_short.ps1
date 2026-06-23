param(
    [string]$BaseDir = "C:\Users\tersi\Documents\Codex\2026-04-24\files-mentioned-by-the-user-poison",
    [string]$Gpp = "C:\msys64\ucrt64\bin\g++.exe",
    [int]$Nr = 20,
    [int]$Tmod = 30,
    [UInt64]$Block = 100000,
    [string]$ThreadsList = "1,16",
    [int]$WarmupRuns = 0,
    [int]$MeasuredRuns = 1,
    [switch]$SkipBuild,
    [string]$OutDir = ""
)

$ErrorActionPreference = "Stop"

if (-not $OutDir) {
    $OutDir = Join-Path $BaseDir "bench_compare_new_vs_modelopt_short"
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

$buildNewV1 = Join-Path $BaseDir "build_seird_strict_v1.ps1"
$buildNewV34 = Join-Path $BaseDir "build_seird_strict_v34.ps1"
$buildModelopt = Join-Path $BaseDir "build_seird_modelopt_v1_v4.ps1"
$benchOpenmp = Join-Path $BaseDir "bench_openmp.ps1"

foreach ($p in @($buildNewV1, $buildNewV34, $buildModelopt, $benchOpenmp)) {
    Assert-Exists $p
}

if (-not $SkipBuild) {
    Log "Building new SEIRD binaries..."
    powershell -ExecutionPolicy Bypass -File $buildNewV1 -Gpp $Gpp
    powershell -ExecutionPolicy Bypass -File $buildNewV34 -Gpp $Gpp

    Log "Building modelopt SEIRD binaries..."
    powershell -ExecutionPolicy Bypass -File $buildModelopt -Gpp $Gpp
}

$exeMap = @(
    @{ family = "new";      core = "v1"; exe = (Join-Path $BaseDir "seird_v1_STRICT.exe") },
    @{ family = "new";      core = "v2"; exe = (Join-Path $BaseDir "seird_v2_STRICT.exe") },
    @{ family = "new";      core = "v3"; exe = (Join-Path $BaseDir "seird_v3_STRICT.exe") },
    @{ family = "new";      core = "v4"; exe = (Join-Path $BaseDir "seird_v4_STRICT.exe") },
    @{ family = "modelopt"; core = "v1"; exe = (Join-Path $BaseDir "seird_modelopt_v1.exe") },
    @{ family = "modelopt"; core = "v2"; exe = (Join-Path $BaseDir "seird_modelopt_v2.exe") },
    @{ family = "modelopt"; core = "v3"; exe = (Join-Path $BaseDir "seird_modelopt_v3.exe") },
    @{ family = "modelopt"; core = "v4"; exe = (Join-Path $BaseDir "seird_modelopt_v4.exe") }
)

foreach ($row in $exeMap) {
    Assert-Exists $row.exe
}

$allRows = @()

foreach ($row in $exeMap) {
    $family = $row.family
    $core = $row.core
    $exe = $row.exe
    $csv = Join-Path $OutDir ("{0}_{1}.csv" -f $family, $core)

    Log ("Running {0}/{1}..." -f $family, $core)
    powershell -ExecutionPolicy Bypass -File $benchOpenmp `
        -Exe $exe `
        -Nr $Nr `
        -Tmod $Tmod `
        -Block $Block `
        -ThreadsList $ThreadsList `
        -WarmupRuns $WarmupRuns `
        -MeasuredRuns $MeasuredRuns `
        -CsvPath $csv

    foreach ($r in (Import-Csv $csv)) {
        $allRows += [pscustomobject]@{
            family       = $family
            core         = $core
            run_id       = [int]$r.run_id
            threads      = [int]$r.threads
            rng_threads  = [int]$r.rng_threads
            rng_prefetch = [UInt64]$r.rng_prefetch
            Nr           = $Nr
            Tmod         = $Tmod
            block        = $Block
            time_total_s = D $r.time_total_s
            speedup      = D $r.speedup
            efficiency   = D $r.efficiency
        }
    }
}

$runsCsv = Join-Path $OutDir "runs.csv"
$allRows | Sort-Object family, core, threads, run_id | Export-Csv -Path $runsCsv -NoTypeInformation -Encoding UTF8

$summary = $allRows |
    Group-Object family, core, threads |
    ForEach-Object {
        $g = $_.Group
        [pscustomobject]@{
            family       = $g[0].family
            core         = $g[0].core
            threads      = $g[0].threads
            rng_threads  = $g[0].rng_threads
            rng_prefetch = $g[0].rng_prefetch
            avg_s        = [Math]::Round(($g | Measure-Object time_total_s -Average).Average, 6)
            min_s        = [Math]::Round(($g | Measure-Object time_total_s -Minimum).Minimum, 6)
            max_s        = [Math]::Round(($g | Measure-Object time_total_s -Maximum).Maximum, 6)
            avg_speedup  = [Math]::Round(($g | Measure-Object speedup -Average).Average, 6)
            avg_eff      = [Math]::Round(($g | Measure-Object efficiency -Average).Average, 6)
        }
    } | Sort-Object core, family, threads

$summaryCsv = Join-Path $OutDir "summary.csv"
$summary | Export-Csv -Path $summaryCsv -NoTypeInformation -Encoding UTF8

Log "Done."
Log "Runs: $runsCsv"
Log "Summary: $summaryCsv"
$summary | Format-Table -AutoSize
