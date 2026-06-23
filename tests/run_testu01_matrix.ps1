param(
    [string]$Gpp = "C:\msys64\ucrt64\bin\g++.exe",
    [string]$IncludeDir = "C:\msys64\ucrt64\include",
    [string]$LibDir = "C:\msys64\ucrt64\lib",
    [string]$Battery = "small",                      # small | crush | big
    [UInt64]$Seed = 1,
    [string]$Generators = "v1,v2,v3,v4,v5,parmonc_lcg,mt19937_64,xoshiro256pp,splitmix64,pcg64",
    [int]$Threads = 0,
    [UInt64]$Chunk = 1048576,
    [switch]$SkipEquivalent,                         # optional: skip v2..v5/parmonc_lcg as sequence-equivalent to v1
    [switch]$SkipBuild,
    [string]$OutDir = ""
)

$ErrorActionPreference = "Stop"

$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Src = Join-Path $ScriptDir "testu01_multi_runner.cpp"
$Exe = Join-Path $ScriptDir "testu01_multi_runner.exe"

if (-not $OutDir) {
    $stamp = Get-Date -Format "yyyyMMdd_HHmmss"
    $OutDir = Join-Path $ScriptDir ("testu01_runs_" + $stamp)
}
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }

$log = Join-Path $OutDir "run_testu01_matrix.log"
if (Test-Path $log) { Remove-Item $log -Force }

function Log($msg) {
    $ts = (Get-Date).ToString("yyyy-MM-dd HH:mm:ss")
    $line = "[$ts] $msg"
    Write-Host $line
    [System.IO.File]::AppendAllText($log, $line + "`r`n", [System.Text.UTF8Encoding]::new($false))
}

function Run-CmdToFile {
    param(
        [string]$ExePath,
        [string[]]$Argv,
        [string]$OutFile
    )
    & $ExePath @Argv *>&1 | Set-Content -Path $OutFile -Encoding UTF8
    return $LASTEXITCODE
}

if (-not $SkipBuild) {
    Log "[build] compiling testu01_multi_runner.cpp"
    if (-not (Test-Path $Gpp)) { throw "g++ не найден: $Gpp" }
    & $Gpp -O3 -std=c++17 -fopenmp -I"$IncludeDir" $Src -o $Exe -L"$LibDir" -ltestu01 -lprobdist -lm 2>&1 |
        Tee-Object -FilePath (Join-Path $OutDir "build_testu01_multi_runner.log") | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "Сборка завершилась ошибкой. См. build_testu01_multi_runner.log." }
    Log "[build] OK -> $Exe"
} else {
    Log "[build] skipped"
}
if (-not (Test-Path $Exe)) { throw "Исполняемый файл не найден: $Exe" }

if ($Battery -notin @("small", "crush", "big")) {
    throw "Параметр Battery должен быть одним из: small, crush, big."
}

$dups = @{
    "v2" = "v1"
    "v3" = "v1"
    "v4" = "v1"
    "v5" = "v1"
    "parmonc_lcg" = "v1"
}

$gens = @($Generators.Split(",") | ForEach-Object { $_.Trim() } | Where-Object { $_ })
$finalGens = New-Object System.Collections.Generic.List[string]

foreach ($g in $gens) {
    if ($SkipEquivalent -and $dups.ContainsKey($g)) {
        Log ("[skip] {0} skipped (sequence-equivalent to {1})" -f $g, $dups[$g])
        continue
    }
    $finalGens.Add($g)
}

if ($finalGens.Count -eq 0) {
    throw "Не осталось генераторов для запуска. Используйте -IncludeDuplicates или скорректируйте -Generators."
}

Log ("[run] battery={0}, seed={1}, threads={2}, chunk={3}, generators={4}" -f $Battery, $Seed, $Threads, $Chunk, ($finalGens -join ", "))

foreach ($g in $finalGens) {
    $outFile = Join-Path $OutDir ("{0}_{1}.log" -f $Battery, $g)
    Log ("[run] gen={0} -> {1}" -f $g, $outFile)
    $argv = @("--gen=$g", "--battery=$Battery", "--seed=$Seed", "--threads=$Threads", "--chunk=$Chunk")
    $code = Run-CmdToFile -ExePath $Exe -Argv $argv -OutFile $outFile
    if ($code -ne 0) {
        Log ("[fail] gen={0}, код выхода={1}" -f $g, $code)
        throw "Запуск завершился ошибкой для генератора: $g"
    }
    Log ("[ok] gen={0}" -f $g)
}

Log "[done] all selected generators completed"
Write-Host ""
Write-Host "Каталог вывода: $OutDir"
Write-Host "Основной лог:         $log"
