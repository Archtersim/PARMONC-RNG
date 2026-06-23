param(
    [string]$Exe = "C:\Users\tersi\Documents\Codex\2026-04-24\files-mentioned-by-the-user-poison\seird_omp.exe",
    [int]$Nr = 20000,
    [int]$Tmod = 90,
    [UInt64]$Block = 1000000,
    [int]$RngThreads = 1,
    [UInt64]$RngPrefetch = 65536,
    [int]$RngSlowdown = 1,
    [string]$ThreadsList = "1,2,4,8,16",
    [int]$WarmupRuns = 1,
    [int]$MeasuredRuns = 1,
    [string]$CsvPath = "C:\Users\tersi\Documents\Codex\2026-04-24\files-mentioned-by-the-user-poison\bench_openmp_results.csv",
    [string]$MsysBin = "C:\msys64\ucrt64\bin"
)

$ErrorActionPreference = "Stop"

$threads = $ThreadsList.Split(",") | ForEach-Object { [int]($_.Trim()) } | Where-Object { $_ -gt 0 }
if ($threads.Count -eq 0) {
    throw "ThreadsList is empty. Example: -ThreadsList '1,2,4,8,16'"
}

if (-not (Test-Path $Exe)) {
    throw "Executable not found: $Exe"
}

if (Test-Path $MsysBin) {
    $env:PATH = "$MsysBin;$env:PATH"
}

function Run-One {
    param(
        [string]$ExePath,
        [int]$Threads,
        [int]$NrArg,
        [int]$TmodArg,
        [UInt64]$BlockArg,
        [int]$RngThreadsArg,
        [UInt64]$RngPrefetchArg,
        [int]$RngSlowdownArg
    )

    $env:OMP_NUM_THREADS = "$Threads"
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $args = @(
        '--Nr', $NrArg,
        '--Tmod', $TmodArg,
        '--threads', $Threads,
        '--rng-threads', $RngThreadsArg,
        '--block', $BlockArg,
        '--rng-prefetch', $RngPrefetchArg
    )
    if ($RngSlowdownArg -gt 1) {
        $args += @('--rng-slowdown', $RngSlowdownArg)
    }
    & $ExePath @args | Out-Null
    $sw.Stop()

    if ($LASTEXITCODE -ne 0) {
        throw "Run failed for threads=$Threads, exit=$LASTEXITCODE"
    }

    return [double]$sw.Elapsed.TotalSeconds
}

$hostname = $env:COMPUTERNAME
$date = (Get-Date).ToString("yyyy-MM-ddTHH:mm:ss")
$results = @()

foreach ($t in $threads) {
    for ($w = 1; $w -le $WarmupRuns; $w++) {
        [void](Run-One -ExePath $Exe -Threads $t -NrArg $Nr -TmodArg $Tmod -BlockArg $Block -RngThreadsArg $RngThreads -RngPrefetchArg $RngPrefetch -RngSlowdownArg $RngSlowdown)
    }

    for ($r = 1; $r -le $MeasuredRuns; $r++) {
        $sec = Run-One -ExePath $Exe -Threads $t -NrArg $Nr -TmodArg $Tmod -BlockArg $Block -RngThreadsArg $RngThreads -RngPrefetchArg $RngPrefetch -RngSlowdownArg $RngSlowdown
        $results += [PSCustomObject]@{
            mode         = "openmp"
            run_id       = $r
            hostname     = $hostname
            date         = $date
            Nr           = $Nr
            Tmod         = $Tmod
            block        = $Block
            rng_threads  = $RngThreads
            rng_prefetch = $RngPrefetch
            rng_slowdown = $RngSlowdown
            ranks        = 1
            threads      = $t
            time_total_s = [Math]::Round($sec, 6)
            speedup      = 0.0
            efficiency   = 0.0
        }
    }
}

$baseline = ($results | Where-Object { $_.threads -eq 1 } | Measure-Object -Property time_total_s -Average).Average
if (-not $baseline) {
    throw "No baseline for threads=1 found."
}

foreach ($row in $results) {
    $s = $baseline / $row.time_total_s
    $e = $s / [double]$row.threads
    $row.speedup = [Math]::Round($s, 6)
    $row.efficiency = [Math]::Round($e, 6)
}

$results |
    Sort-Object threads, run_id |
    Export-Csv -Path $CsvPath -NoTypeInformation -Encoding UTF8

$summary = $results |
    Group-Object threads |
    ForEach-Object {
        $avg = ($_.Group | Measure-Object -Property time_total_s -Average).Average
        $min = ($_.Group | Measure-Object -Property time_total_s -Minimum).Minimum
        $max = ($_.Group | Measure-Object -Property time_total_s -Maximum).Maximum
        [PSCustomObject]@{
            threads  = [int]$_.Name
            avg_s    = [Math]::Round($avg, 6)
            min_s    = [Math]::Round($min, 6)
            max_s    = [Math]::Round($max, 6)
            speedup  = [Math]::Round(($baseline / $avg), 6)
            eff      = [Math]::Round((($baseline / $avg) / [int]$_.Name), 6)
        }
    } |
    Sort-Object threads

"CSV saved to: $CsvPath"
$summary | Format-Table -AutoSize
