param(
    [string]$BaseDir = "",
    [string]$SlowdownList = "1,2,4,8,16,32",
    [string]$ThreadsList = "1,16",
    [int]$Nr = 100,
    [int]$Tmod = 90,
    [UInt64]$Block = 1000000,
    [int]$WarmupRuns = 1,
    [int]$MeasuredRuns = 3
)

$ErrorActionPreference = "Stop"

if (-not $BaseDir) {
    $BaseDir = $PSScriptRoot
}

$buildPs = Join-Path $BaseDir "build_seird_strict_v34.ps1"
$benchPs = Join-Path $BaseDir "bench_openmp.ps1"
$seirdDir = Join-Path (Split-Path $PSScriptRoot -Parent) "seird"
$exeV3 = Join-Path $seirdDir "seird_v3_STRICT.exe"
$exeV4 = Join-Path $seirdDir "seird_v4_STRICT.exe"
$outDir = Join-Path $BaseDir "bench_seird_rng_saturation"
if (-not (Test-Path $outDir)) { New-Item -ItemType Directory -Path $outDir | Out-Null }

if (-not (Test-Path $buildPs)) { throw "Отсутствует: $buildPs" }
if (-not (Test-Path $benchPs)) { throw "Отсутствует: $benchPs" }

powershell -ExecutionPolicy Bypass -File $buildPs

$slowdowns = $SlowdownList.Split(",") | ForEach-Object { [int]($_.Trim()) } | Where-Object { $_ -gt 0 }
if ($slowdowns.Count -eq 0) { throw "Список SlowdownList пуст." }

foreach ($k in $slowdowns) {
    $csvV3 = Join-Path $outDir ("bench_v3_sd{0}.csv" -f $k)
    $csvV4 = Join-Path $outDir ("bench_v4_sd{0}.csv" -f $k)

    powershell -ExecutionPolicy Bypass -File $benchPs `
        -Exe $exeV3 -Nr $Nr -Tmod $Tmod -Block $Block -RngSlowdown $k `
        -ThreadsList $ThreadsList -WarmupRuns $WarmupRuns -MeasuredRuns $MeasuredRuns `
        -CsvPath $csvV3

    powershell -ExecutionPolicy Bypass -File $benchPs `
        -Exe $exeV4 -Nr $Nr -Tmod $Tmod -Block $Block -RngSlowdown $k `
        -ThreadsList $ThreadsList -WarmupRuns $WarmupRuns -MeasuredRuns $MeasuredRuns `
        -CsvPath $csvV4
}

# Build compact summary across all slowdown points.
$rows = @()
foreach ($k in $slowdowns) {
    $csvV3 = Join-Path $outDir ("bench_v3_sd{0}.csv" -f $k)
    $csvV4 = Join-Path $outDir ("bench_v4_sd{0}.csv" -f $k)
    $v3 = Import-Csv $csvV3
    $v4 = Import-Csv $csvV4
    function D([string]$x) { return [double](($x -replace ",",".")) }
    $v3t1  = (($v3 | Where-Object {[int]$_.threads -eq 1}  | ForEach-Object { D $_.time_total_s } | Measure-Object -Average).Average)
    $v3t16 = (($v3 | Where-Object {[int]$_.threads -eq 16} | ForEach-Object { D $_.time_total_s } | Measure-Object -Average).Average)
    $v4t1  = (($v4 | Where-Object {[int]$_.threads -eq 1}  | ForEach-Object { D $_.time_total_s } | Measure-Object -Average).Average)
    $v4t16 = (($v4 | Where-Object {[int]$_.threads -eq 16} | ForEach-Object { D $_.time_total_s } | Measure-Object -Average).Average)
    $rows += [PSCustomObject]@{
        rng_slowdown = $k
        v3_t1_s = [Math]::Round($v3t1, 6)
        v3_t16_s = [Math]::Round($v3t16, 6)
        v4_t1_s = [Math]::Round($v4t1, 6)
        v4_t16_s = [Math]::Round($v4t16, 6)
        speedup_v3_to_v4_t1 = [Math]::Round($v3t1 / $v4t1, 6)
        speedup_v3_to_v4_t16 = [Math]::Round($v3t16 / $v4t16, 6)
    }
}

$summaryCsv = Join-Path $outDir "saturation_summary.csv"
$rows | Sort-Object rng_slowdown | Export-Csv -Path $summaryCsv -NoTypeInformation -Encoding UTF8

Write-Host "Сводка сохранена: $summaryCsv"
$rows | Sort-Object rng_slowdown | Format-Table -AutoSize
