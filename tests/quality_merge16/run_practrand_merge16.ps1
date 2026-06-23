param(
    [string]$Gpp = "C:\msys64\ucrt64\bin\g++.exe",
    [string]$MsysBin = "C:\msys64\ucrt64\bin",
    [string]$PractRandExe = "",
    [string]$Generator = "mt19937_64",
    [UInt64]$Seed = 3405643776,
    [int]$Threads = 16,
    [string]$Merge = "roundrobin",
    [UInt64]$BlockWords = 4096,
    [string]$MinBytes = "1GB",
    [string]$MaxBytes = "32GB",
    [int]$Foldings = 2,
    [switch]$SkipBuild,
    [string]$OutDir = ""
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Src = Join-Path $ScriptDir "rng_streamer_merge16.cpp"
$Exe = Join-Path $ScriptDir "rng_streamer_merge16.exe"

if (-not $OutDir) {
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $OutDir = Join-Path $ScriptDir ("practrand_merge16_" + $stamp)
}
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }
$LogFile = Join-Path $OutDir ("{0}_{1}.log" -f $Generator, $Merge)

if (Test-Path $MsysBin) { $env:PATH = "$MsysBin;$env:PATH" }
if (-not $PractRandExe) {
    $cmd = Get-Command "RNG_test.exe" -ErrorAction SilentlyContinue
    if ($cmd) { $PractRandExe = $cmd.Source }
}
if (-not $PractRandExe -or -not (Test-Path $PractRandExe)) {
    throw "Не найден PractRand RNG_test.exe. Укажите путь через -PractRandExe."
}

if (-not $SkipBuild) {
    & $Gpp -O3 -std=c++17 $Src -o $Exe 2>&1 |
        Tee-Object -FilePath (Join-Path $OutDir "build_practrand_merge16.log") | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Сборка завершилась ошибкой." }
}

$cmdLine = "`"`"$Exe`" --gen=$Generator --seed=$Seed --threads=$Threads --merge=$Merge --block-words=$BlockWords | `"$PractRandExe`" stdin64 -tf $Foldings -tlmin $MinBytes -tlmax $MaxBytes`""
$tmpOut = [System.IO.Path]::GetTempFileName()
$tmpErr = [System.IO.Path]::GetTempFileName()
try {
    $proc = Start-Process -FilePath "cmd.exe" `
                          -ArgumentList @("/c", $cmdLine) `
                          -RedirectStandardOutput $tmpOut `
                          -RedirectStandardError $tmpErr `
                          -NoNewWindow -PassThru -Wait
    $stdout = if (Test-Path $tmpOut) { Get-Content $tmpOut -Raw -Encoding UTF8 } else { "" }
    $stderr = if (Test-Path $tmpErr) { Get-Content $tmpErr -Raw -Encoding UTF8 } else { "" }
    ($stdout + "`r`n=== STDERR ===`r`n" + $stderr) | Set-Content -Path $LogFile -Encoding UTF8
    if ($proc.ExitCode -ne 0) {
        throw "Конвейер PractRand завершился с кодом $($proc.ExitCode)"
    }
}
finally {
    Remove-Item $tmpOut, $tmpErr -Force -ErrorAction SilentlyContinue
}

Write-Host "Выходной файл: $LogFile"

