param(
    [int]$Nr = 100,
    [int]$Tmod = 90,
    [UInt64]$Block = 1000000,
    [int]$RngThreads = 16,
    [UInt64]$RngPrefetch = 65536,
    [int]$WarmupRuns = 1,
    [int]$MeasuredRuns = 3
)

$ErrorActionPreference = "Stop"

$base = Split-Path $PSScriptRoot -Parent
$seird = Join-Path $base "seird"
$bench = Join-Path $PSScriptRoot "bench_openmp.ps1"
$outDir = Join-Path $PSScriptRoot "bench_model1_rng16"
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }

$map = @{
    "v1" = "seird_v1_STRICT.exe"
    "v2" = "seird_v2_STRICT.exe"
    "v3" = "seird_v3_STRICT.exe"
    "v4" = "seird_v4_STRICT.exe"
}

foreach ($k in $map.Keys) {
    $exe = Join-Path $seird $map[$k]
    if (-not (Test-Path $exe)) {
        throw "Отсутствует исполняемый файл: $exe. Сначала соберите строгие бинарные файлы SEIRD."
    }
    $csv = Join-Path $outDir ("{0}_model1_rng{1}.csv" -f $k, $RngThreads)
    powershell -ExecutionPolicy Bypass -File $bench `
        -Exe $exe `
        -Nr $Nr `
        -Tmod $Tmod `
        -Block $Block `
        -ThreadsList "1" `
        -RngThreads $RngThreads `
        -RngPrefetch $RngPrefetch `
        -WarmupRuns $WarmupRuns `
        -MeasuredRuns $MeasuredRuns `
        -CsvPath $csv
}

Write-Host "CSV-файлы бенчмарка сохранены в: $outDir"
