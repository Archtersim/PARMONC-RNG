param(
    [string]$Gpp = "C:\msys64\ucrt64\bin\g++.exe",
    [string]$StandaloneDir = ""
)

$ErrorActionPreference = "Stop"

if (-not $StandaloneDir) {
    $StandaloneDir = Join-Path (Split-Path $PSScriptRoot -Parent) "src"
}

if (-not (Test-Path $Gpp)) { throw "g++ не найден: $Gpp" }

$v1 = Join-Path $StandaloneDir "v1_rng_seq.cpp"
$v2 = Join-Path $StandaloneDir "v2_rng_par.cpp"
$v3 = Join-Path $StandaloneDir "v3_rng_par_v3.cpp"
$v4 = Join-Path $StandaloneDir "v4_rng_par_v4.cpp"

foreach ($f in @($v1, $v2, $v3, $v4)) {
    if (-not (Test-Path $f)) { throw "Отсутствует исходный файл: $f" }
}

& $Gpp -O3 -std=c++17 $v1 -o (Join-Path $StandaloneDir "rng_seq.exe")
if ($LASTEXITCODE -ne 0) { throw "Сборка завершилась ошибкой: rng_seq.exe" }

& $Gpp -O3 -std=c++17 -fopenmp $v2 -o (Join-Path $StandaloneDir "rng_par.exe")
if ($LASTEXITCODE -ne 0) { throw "Сборка завершилась ошибкой: rng_par.exe" }

& $Gpp -O3 -std=c++17 -fopenmp $v3 -o (Join-Path $StandaloneDir "rng_par_v3.exe")
if ($LASTEXITCODE -ne 0) { throw "Сборка завершилась ошибкой: rng_par_v3.exe" }

& $Gpp -O3 -std=c++17 -fopenmp -mavx2 -mbmi2 -mfma $v4 -o (Join-Path $StandaloneDir "rng_par_v4.exe")
if ($LASTEXITCODE -ne 0) { throw "Сборка завершилась ошибкой: rng_par_v4.exe" }

Write-Host "Собраны автономные бинарные файлы v1-v4 в $StandaloneDir"
