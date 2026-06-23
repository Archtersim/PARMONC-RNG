param(
    [string]$Gpp = "C:\msys64\ucrt64\bin\g++.exe",
    [string]$IncludeDir = "C:\msys64\ucrt64\include",
    [string]$LibDir = "C:\msys64\ucrt64\lib",
    [string]$Battery = "small",
    [string]$Generator = "mt19937_64",
    [UInt64]$Seed = 3405643776,
    [int]$Threads = 16,
    [string]$Merge = "roundrobin",
    [UInt64]$BlockWords = 4096,
    [switch]$SkipBuild,
    [string]$OutDir = ""
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Src = Join-Path $ScriptDir "testu01_merge16_runner.cpp"
$Exe = Join-Path $ScriptDir "testu01_merge16_runner.exe"

if (-not $OutDir) {
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $OutDir = Join-Path $ScriptDir ("testu01_merge16_" + $stamp)
}
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }

if (-not $SkipBuild) {
    & $Gpp -O3 -std=c++17 -I"$IncludeDir" $Src -o $Exe -L"$LibDir" -ltestu01 -lprobdist -lm 2>&1 |
        Tee-Object -FilePath (Join-Path $OutDir "build_testu01_merge16.log") | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Сборка завершилась ошибкой." }
}

$OutFile = Join-Path $OutDir ("{0}_{1}_{2}.log" -f $Battery, $Generator, $Merge)
& $Exe "--gen=$Generator" "--battery=$Battery" "--seed=$Seed" "--threads=$Threads" "--merge=$Merge" "--block-words=$BlockWords" *>&1 |
    Set-Content -Path $OutFile -Encoding UTF8

Write-Host "Выходной файл: $OutFile"

