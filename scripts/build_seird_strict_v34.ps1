param(
    [string]$Gpp = "C:\msys64\ucrt64\bin\g++.exe",
    [string]$BaseDir = ""
)

$ErrorActionPreference = "Stop"

if (-not $BaseDir) {
    $BaseDir = Join-Path (Split-Path $PSScriptRoot -Parent) "seird"
}

$src = Join-Path $BaseDir "seird_hybrid_mpi_omp.cpp"
$exeV2 = Join-Path $BaseDir "seird_v2_STRICT.exe"
$exeV3 = Join-Path $BaseDir "seird_v3_STRICT.exe"
$exeV4 = Join-Path $BaseDir "seird_v4_STRICT.exe"

if (-not (Test-Path $Gpp)) { throw "g++ не найден: $Gpp" }
if (-not (Test-Path $src)) { throw "исходный файл не найден: $src" }

& $Gpp -O3 -march=native -std=c++17 -fopenmp -DRNG_V2_MANUAL_MUL $src -o $exeV2
if ($LASTEXITCODE -ne 0) { throw "Сборка завершилась ошибкой: $exeV2" }

& $Gpp -O3 -march=native -std=c++17 -fopenmp $src -o $exeV3
if ($LASTEXITCODE -ne 0) { throw "Сборка завершилась ошибкой: $exeV3" }

& $Gpp -O3 -march=native -std=c++17 -fopenmp -DRNG_V4_BUFFERED -mavx2 -mbmi2 -mfma $src -o $exeV4
if ($LASTEXITCODE -ne 0) { throw "Сборка завершилась ошибкой: $exeV4" }

Write-Host "Собраны строгие бинарные файлы SEIRD:"
Get-Item $exeV2, $exeV3, $exeV4 | Select-Object Name, Length, LastWriteTime | Format-Table -AutoSize

Write-Host ""
Write-Host "Быстрая проверка:"
& $exeV2 --Nr 1 --Tmod 1 --threads 1 --block 1000
& $exeV3 --Nr 1 --Tmod 1 --threads 1 --block 1000
& $exeV4 --Nr 1 --Tmod 1 --threads 1 --block 1000
