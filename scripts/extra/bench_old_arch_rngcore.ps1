param(
    [string]$Exe = "C:\Users\tersi\Documents\Codex\2026-04-24\files-mentioned-by-the-user-poison\old_profiled_rngcore_compare.exe",
    [string]$Cores = "v1,v2,v3,v4",
    [int]$Nr = 100,
    [int]$Tmod = 90,
    [double]$Coef = 1.0,
    [int]$WarmupRuns = 1,
    [int]$MeasuredRuns = 3,
    [string]$CsvPath = "C:\Users\tersi\Documents\Codex\2026-04-24\files-mentioned-by-the-user-poison\bench_old_arch_rngcore.csv"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $Exe)) {
    throw "Executable not found: $Exe"
}

$coreList = $Cores.Split(",") | ForEach-Object { $_.Trim().ToLowerInvariant() } | Where-Object { $_ -ne "" }
if ($coreList.Count -eq 0) {
    throw "Empty -Cores list. Example: -Cores 'v1,v2,v3,v4'"
}

function Run-One {
    param(
        [string]$ExePath,
        [string]$Core,
        [int]$NrArg,
        [int]$TmodArg,
        [double]$CoefArg
    )
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    & $ExePath --rng-core $Core --Nr $NrArg --Tmod $TmodArg --coef $CoefArg | Out-Null
    $sw.Stop()
    if ($LASTEXITCODE -ne 0) {
        throw "Run failed for core=$Core, exit=$LASTEXITCODE"
    }
    return [double]$sw.Elapsed.TotalSeconds
}

$hostname = $env:COMPUTERNAME
$date = (Get-Date).ToString("yyyy-MM-ddTHH:mm:ss")
$rows = @()

foreach ($core in $coreList) {
    for ($w = 1; $w -le $WarmupRuns; $w++) {
        [void](Run-One -ExePath $Exe -Core $core -NrArg $Nr -TmodArg $Tmod -CoefArg $Coef)
    }
    for ($r = 1; $r -le $MeasuredRuns; $r++) {
        $sec = Run-One -ExePath $Exe -Core $core -NrArg $Nr -TmodArg $Tmod -CoefArg $Coef
        $rows += [PSCustomObject]@{
            mode         = "old_arch_rngcore"
            core         = $core
            run_id       = $r
            hostname     = $hostname
            date         = $date
            Nr           = $Nr
            Tmod         = $Tmod
            coef         = $Coef
            time_total_s = [Math]::Round($sec, 6)
            speedup_vs_v1 = 0.0
        }
    }
}

$v1Base = ($rows | Where-Object { $_.core -eq "v1" } | Measure-Object -Property time_total_s -Average).Average
if (-not $v1Base) {
    throw "No v1 baseline in selected cores. Include v1 in -Cores."
}

foreach ($row in $rows) {
    $row.speedup_vs_v1 = [Math]::Round($v1Base / $row.time_total_s, 6)
}

$rows | Sort-Object core, run_id | Export-Csv -Path $CsvPath -NoTypeInformation -Encoding UTF8

$summary = $rows |
    Group-Object core |
    ForEach-Object {
        $avg = ($_.Group | Measure-Object -Property time_total_s -Average).Average
        $min = ($_.Group | Measure-Object -Property time_total_s -Minimum).Minimum
        $max = ($_.Group | Measure-Object -Property time_total_s -Maximum).Maximum
        [PSCustomObject]@{
            core = $_.Name
            avg_s = [Math]::Round($avg, 6)
            min_s = [Math]::Round($min, 6)
            max_s = [Math]::Round($max, 6)
            speedup_vs_v1 = [Math]::Round(($v1Base / $avg), 6)
        }
    } |
    Sort-Object core

"CSV saved to: $CsvPath"
$summary | Format-Table -AutoSize
