param(
    [string]$Gpp = "C:\msys64\ucrt64\bin\g++.exe",
    [string]$BaseDir = ""
)

$ErrorActionPreference = "Stop"

if (-not $BaseDir) {
    $BaseDir = Join-Path (Split-Path $PSScriptRoot -Parent) "seird"
}

$src = Join-Path $BaseDir "seird_hybrid_mpi_omp.cpp"
$exeV1 = Join-Path $BaseDir "seird_v1_STRICT.exe"

if (-not (Test-Path $Gpp)) { throw "g++ не найден: $Gpp" }
if (-not (Test-Path $src)) { throw "исходный файл не найден: $src" }

& $Gpp -O3 -march=native -std=c++17 -fopenmp -DRNG_V1_DIGIT_STEP $src -o $exeV1
if ($LASTEXITCODE -ne 0) { throw "Сборка завершилась ошибкой: $exeV1" }

Write-Host "Собрано:"
Get-Item $exeV1 | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize

Write-Host ""
Write-Host "Быстрая проверка:"
& $exeV1 --Nr 1 --Tmod 1 --threads 1 --block 1000
