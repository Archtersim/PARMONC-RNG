# bench_cuda.ps1
#
# Builds and benchmarks the CUDA implementation rng_par_v5.cu against
# rng_par_v4.exe (best CPU version) on the same N matrix as bench_rng3 /
# bench_rng4. The bench can also be run in compare mode against any
# other CPU exe via -CompareExe.
#
# nvcc on Windows internally calls cl.exe (the MSVC host compiler) for
# every host-side translation unit. If cl.exe is not on PATH, nvcc dies
# with "Cannot find compiler 'cl.exe' in PATH". This script auto-detects
# vcvars64.bat (via vswhere) and imports the MSVC environment into the
# current process before invoking nvcc -- same trick we use in bench_mpi.
#
# Output in $WorkDir:
#   bench_cuda_results.csv  -- raw timings per measured run
#   bench_cuda_summary.csv  -- avg/min/max + throughput + GPU/CPU ratio
#   bench_cuda.log
#   sysinfo.txt             -- includes the [v5] GPU detect line
#
# Run examples:
#   powershell -ExecutionPolicy Bypass -File .\bench_cuda.ps1
#   .\bench_cuda.ps1 -Arch sm_86 -NList "1000000,10000000,100000000"
#   .\bench_cuda.ps1 -SkipBuild
#   .\bench_cuda.ps1 -DryRun
#
# Auto-detection: if -Arch is empty, the script asks nvcc to compile a
# fat binary covering compute_75 through compute_90 -- works on any
# Turing+ GPU. Pass an explicit -Arch (sm_75/sm_86/sm_89/sm_90) for
# faster builds and faster first kernel launch.

param(
    [string]$Nvcc        = "",      # auto-detect via PATH or CUDA_PATH
    [string]$VcvarsPath  = "",      # auto-detect via vswhere
    [string]$Arch        = "",      # e.g. "sm_86"; empty => fat binary
    [string]$WorkDir     = "$PSScriptRoot\bench_cuda",
    [string]$ParallelDir = "$((Split-Path $PSScriptRoot -Parent))\standalone",
    [string]$CompareExe  = "$((Split-Path $PSScriptRoot -Parent))\standalone\rng_par_v4.exe",
    [string]$NList       = "1000000,10000000,100000000,500000000",
    [int]$WarmupRuns     = 1,
    [int]$MeasuredRuns   = 3,
    [int]$BlockSize      = 256,     # CUDA threads per block
    [int]$CompareThreads = 20,      # CPU threads for the v4 comparison
    [double]$ChecksumTol = 1e-3,
    [switch]$SkipBuild,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $WorkDir)) { New-Item -ItemType Directory -Path $WorkDir | Out-Null }
$BinDir  = Join-Path $WorkDir "_bin"
$DumpDir = Join-Path $WorkDir "_dump"
foreach ($d in @($BinDir, $DumpDir)) {
    if (-not (Test-Path $d)) { New-Item -ItemType Directory -Path $d | Out-Null }
}

$LogPath = Join-Path $WorkDir "bench_cuda.log"
if (Test-Path $LogPath) { Remove-Item $LogPath -Force }
function Log($msg) {
    $ts = (Get-Date).ToString("yyyy-MM-dd HH:mm:ss")
    $line = "[$ts] $msg"
    Write-Host $line
    [System.IO.File]::AppendAllText($LogPath, $line + "`r`n", [System.Text.UTF8Encoding]::new($false))
}

Log "=== bench_cuda.ps1 started ==="
Log "PSVersion    = $($PSVersionTable.PSVersion)"
Log "WorkDir      = $WorkDir"
Log "ParallelDir  = $ParallelDir"
Log "NList        = $NList"
Log "BlockSize    = $BlockSize"
Log "Arch         = $(if ($Arch) {$Arch} else {'<fat binary>'})"
Log "CompareExe   = $CompareExe"
Log "CompareThreads = $CompareThreads"

# --------------------------------------------------------------------------
# Generic helper: run a native binary, capture stdout and stderr cleanly
# (PS 5.1 quirk: under $ErrorActionPreference='Stop' any text written to
# stderr by a native process becomes a NativeCommandError; Start-Process
# with explicit redirect bypasses this).
# --------------------------------------------------------------------------
function Invoke-NativeCapture {
    param([string]$Exe, [string[]]$ArgList)
    $tmpOut = [System.IO.Path]::GetTempFileName()
    $tmpErr = [System.IO.Path]::GetTempFileName()
    try {
        $proc = Start-Process -FilePath $Exe -ArgumentList $ArgList `
                              -RedirectStandardOutput $tmpOut `
                              -RedirectStandardError  $tmpErr `
                              -NoNewWindow -PassThru -Wait
        $stdout = if (Test-Path $tmpOut) { @(Get-Content $tmpOut -Encoding UTF8 | Where-Object { $_ -match '\S' }) } else { @() }
        $stderr = if (Test-Path $tmpErr) { @(Get-Content $tmpErr -Encoding UTF8 | Where-Object { $_ -match '\S' }) } else { @() }
        return [PSCustomObject]@{
            ExitCode = $proc.ExitCode
            Stdout   = $stdout
            Stderr   = $stderr
        }
    }
    finally {
        Remove-Item $tmpOut, $tmpErr -Force -ErrorAction SilentlyContinue
    }
}

# --------------------------------------------------------------------------
# Locate nvcc
# --------------------------------------------------------------------------
function Find-Nvcc {
    if ($Nvcc -and (Test-Path $Nvcc)) { return $Nvcc }
    $cmd = Get-Command nvcc.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    if ($env:CUDA_PATH) {
        $candidate = Join-Path $env:CUDA_PATH "bin\nvcc.exe"
        if (Test-Path $candidate) { return $candidate }
    }
    $globs = @(
        "C:\Program Files\NVIDIA GPU Computing Toolkit\CUDA\v*\bin\nvcc.exe"
    )
    foreach ($g in $globs) {
        $hit = Get-ChildItem -Path $g -ErrorAction SilentlyContinue |
               Sort-Object FullName -Descending | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    return $null
}

$Nvcc = Find-Nvcc
if (-not $Nvcc) {
    throw "nvcc.exe not found. Install CUDA Toolkit, or pass -Nvcc 'C:\...\nvcc.exe'."
}
Log "nvcc = $Nvcc"

# --------------------------------------------------------------------------
# Locate and import vcvars64.bat (so nvcc can find cl.exe)
# --------------------------------------------------------------------------
function Find-Vcvars64 {
    $candidates = @()
    $vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $installPath = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2>$null
        if ($installPath) {
            $candidates += Join-Path $installPath "VC\Auxiliary\Build\vcvars64.bat"
        }
    }
    $candidates += @(
        "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Auxiliary\Build\vcvars64.bat",
        "C:\Program Files (x86)\Microsoft Visual Studio\2019\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
    )
    foreach ($c in $candidates) { if (Test-Path $c) { return $c } }
    return $null
}

function Import-Vcvars($vcvarsBat) {
    $tmp = [System.IO.Path]::GetTempFileName()
    try {
        cmd /c "`"$vcvarsBat`" >nul 2>&1 && set" > $tmp
        if ($LASTEXITCODE -ne 0) { throw "vcvars64.bat failed (exit $LASTEXITCODE)." }
        Get-Content $tmp | ForEach-Object {
            if ($_ -match '^([^=]+)=(.*)$') {
                [System.Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process')
            }
        }
    }
    finally { Remove-Item $tmp -Force -ErrorAction SilentlyContinue }
}

if (-not $SkipBuild) {
    $cl = Get-Command cl.exe -ErrorAction SilentlyContinue
    if (-not $cl) {
        if (-not $VcvarsPath) { $VcvarsPath = Find-Vcvars64 }
        if (-not $VcvarsPath -or -not (Test-Path $VcvarsPath)) {
            throw "cl.exe not on PATH and vcvars64.bat not found. Either run from 'x64 Native Tools Command Prompt for VS', or pass -VcvarsPath '...\vcvars64.bat'."
        }
        Log "Activating MSVC env via $VcvarsPath ..."
        Import-Vcvars $VcvarsPath
        $cl = Get-Command cl.exe -ErrorAction SilentlyContinue
        if (-not $cl) { throw "cl.exe still not available after vcvars64.bat." }
    }
    Log "cl  = $($cl.Source)"
}

# --------------------------------------------------------------------------
# Build
#
# IMPORTANT: PowerShell's Start-Process splits each "string" element of
# -ArgumentList back into a Win32 command line. Bare host-flag arguments
# like "/EHsc /O2" passed via -Xcompiler get re-tokenized and confuse
# nvcc (it ends up trying to interpret "/O2" as a separate input file
# and converts the leading slash into "C:/O2", which it cannot handle).
#
# The robust form documented by NVIDIA is the equals-sign syntax:
#   -Xcompiler=/EHsc
#   -Xcompiler=/O2
# Each is a single token, no embedded spaces, no ambiguity.
# --------------------------------------------------------------------------
$Src5 = Join-Path $ParallelDir "v5_rng_par_v5.cu"
if (-not (Test-Path $Src5)) { throw "Source not found: $Src5" }
$Exe5 = Join-Path $BinDir "rng_par_v5.exe"

if (-not $SkipBuild) {
    Log "[build] nvcc -O3 -std=c++17 --use_fast_math ..."
    $nvccArgs = @(
        "-O3", "-std=c++17", "--use_fast_math",
        "-allow-unsupported-compiler",
        "-Xcompiler=/EHsc",
        "-Xcompiler=/O2"
    )
    if ($Arch) {
        $nvccArgs += @("-arch=$Arch")
    } else {
        # fat binary: covers Turing through Hopper.
        # -arch=compute_XX builds PTX (forward-compatible JIT),
        # -code=sm_XX builds SASS (native, fastest startup).
        $nvccArgs += @(
            "-gencode=arch=compute_75,code=sm_75",
            "-gencode=arch=compute_80,code=sm_80",
            "-gencode=arch=compute_86,code=sm_86",
            "-gencode=arch=compute_89,code=sm_89",
            "-gencode=arch=compute_90,code=sm_90",
            "-gencode=arch=compute_90,code=compute_90"
        )
    }
    $nvccArgs += @("-o", "$Exe5", "$Src5")
    Log ("  $Nvcc " + ($nvccArgs -join " "))

    $build = Invoke-NativeCapture -Exe $Nvcc -ArgList $nvccArgs
    foreach ($line in $build.Stderr) { Log "  nvcc: $line" }
    if ($build.ExitCode -ne 0) {
        $logFile = Join-Path $WorkDir "build_v5.log"
        ($build.Stdout + $build.Stderr) | Set-Content $logFile -Encoding UTF8
        throw "Build v5 failed (exit $($build.ExitCode)). See $logFile."
    }
    Log "[build] OK -> $Exe5"
} else {
    Log "[build] skipped"
}
if (-not (Test-Path $Exe5)) { throw "Executable missing: $Exe5" }

# --------------------------------------------------------------------------
# Probe: detect the GPU and the launch geometry v5 picks for a small N
# --------------------------------------------------------------------------
$probeOut = Join-Path $DumpDir "probe.txt"
$probe = Invoke-NativeCapture -Exe $Exe5 -ArgList @("1000", "$probeOut", "$BlockSize")
$v5_stderr = $probe.Stderr
foreach ($line in $v5_stderr) { Log "v5 startup: $line" }
if ($probe.ExitCode -ne 0) {
    Log "WARN: v5 probe exit code = $($probe.ExitCode)"
    Log "  stderr was: $($probe.Stderr -join '; ')"
}

# --------------------------------------------------------------------------
# Parse N list
# --------------------------------------------------------------------------
$Nset = @()
foreach ($n in $NList.Split(",")) {
    $s = $n.Trim()
    if (-not $s) { continue }
    $Nset += [UInt64]$s
}
if ($Nset.Count -eq 0) { throw "NList is empty." }
Log ("Parsed N = " + ($Nset -join ", "))

# --------------------------------------------------------------------------
# Helpers
# --------------------------------------------------------------------------
function Parse-StdoutLine {
    param([string]$line, [string]$tag)
    $rx = "^$tag\s+N=(?<n>\d+)\s+T=(?<t>\d+)\s+time_ms=(?<ms>[-\d.]+)\s+checksum=(?<cs>[-\d.]+)\s*$"
    $m = [regex]::Match($line, $rx)
    if (-not $m.Success) { return $null }
    [PSCustomObject]@{
        N        = [UInt64]$m.Groups['n'].Value
        Threads  = [UInt64]$m.Groups['t'].Value
        TimeMs   = [double]$m.Groups['ms'].Value
        Checksum = [double]$m.Groups['cs'].Value
    }
}

function Run-V5 {
    param([UInt64]$N, [string]$DumpFile)
    $r = Invoke-NativeCapture -Exe $Exe5 -ArgList @("$N", "$DumpFile", "$BlockSize")
    if ($r.ExitCode -ne 0) {
        throw "v5 failed for N=$N (exit $($r.ExitCode)). Stderr: $($r.Stderr -join '; ')"
    }
    foreach ($line in $r.Stdout) {
        $parsed = Parse-StdoutLine -line "$line" -tag "PAR5"
        if ($parsed) { return $parsed }
    }
    throw "Could not parse v5 stdout for N=$N. Got: $($r.Stdout -join ' | ')"
}

function Run-V4 {
    param([UInt64]$N, [int]$T, [string]$DumpFile)
    if (-not (Test-Path $CompareExe)) {
        return $null
    }
    $r = Invoke-NativeCapture -Exe $CompareExe -ArgList @("$N", "$DumpFile", "$T")
    if ($r.ExitCode -ne 0) {
        throw "v4 failed for N=$N T=$T (exit $($r.ExitCode))."
    }
    foreach ($line in $r.Stdout) {
        $parsed = Parse-StdoutLine -line "$line" -tag "PAR4"
        if ($parsed) { return $parsed }
    }
    throw "Could not parse v4 stdout for N=$N T=$T."
}

# --------------------------------------------------------------------------
# Sanity check: v5 and v4 produce equivalent numbers (within tol)
# --------------------------------------------------------------------------
$sanN = [UInt64]1000000
Log "[sanity] checksum equality at N=$sanN"
$cmpAvail = Test-Path $CompareExe
$s5 = Run-V5 -N $sanN -DumpFile (Join-Path $DumpDir "sanity_v5.txt")
Log ("  v5 checksum = {0:F6}  (T={1})" -f $s5.Checksum, $s5.Threads)

if ($cmpAvail) {
    $s4 = Run-V4 -N $sanN -T 1 -DumpFile (Join-Path $DumpDir "sanity_v4.txt")
    Log ("  v4 checksum = {0:F6}" -f $s4.Checksum)
    $d = [Math]::Abs($s5.Checksum - $s4.Checksum)
    Log ("  |v5 - v4|  = {0:E3}, tol = {1:E3}" -f $d, $ChecksumTol)
    if ($d -gt $ChecksumTol) {
        throw "Sanity FAILED: v5 and v4 produce different sequences."
    }
    Log "[sanity] OK -- v5 and v4 sequences match"
} else {
    Log "[sanity] WARN: $CompareExe missing, skipped CPU comparison"
}

# --------------------------------------------------------------------------
# sysinfo
# --------------------------------------------------------------------------
$sysinfo = @(
    "===== bench_cuda: system info =====",
    "Run date:               $((Get-Date).ToString('s'))",
    "Computer:               $env:COMPUTERNAME",
    "OS:                     $env:OS",
    "PROCESSOR_IDENTIFIER:   $env:PROCESSOR_IDENTIFIER",
    "NUMBER_OF_PROCESSORS:   $env:NUMBER_OF_PROCESSORS",
    "nvcc:                   $Nvcc",
    ""
)
foreach ($line in $v5_stderr) { $sysinfo += $line }
$sysinfo += ""
try {
    $cpu = wmic cpu get name /format:list 2>$null | Where-Object { $_ -match "=" }
    $sysinfo += "--- CPU ---"
    $sysinfo += $cpu
} catch {}
$sysinfo | Set-Content (Join-Path $WorkDir "sysinfo.txt") -Encoding UTF8

# --------------------------------------------------------------------------
# Plan
# --------------------------------------------------------------------------
$Plan = @()
foreach ($n in $Nset) {
    $Plan += [PSCustomObject]@{ Version="v5_cuda"; N=$n; Threads="GPU"; Exe=$Exe5;       Tag="PAR5" }
    if ($cmpAvail) {
        $Plan += [PSCustomObject]@{ Version="v4_cpu";  N=$n; Threads=$CompareThreads; Exe=$CompareExe; Tag="PAR4" }
    }
}
$totalRuns = $Plan.Count * ($WarmupRuns + $MeasuredRuns)
Log ("Plan: {0} configurations, {1} runs each = {2} total runs" -f $Plan.Count, ($WarmupRuns + $MeasuredRuns), $totalRuns)

if ($DryRun) {
    foreach ($p in $Plan) {
        Log ("  {0,-8}  N={1,-12}  T={2}" -f $p.Version, $p.N, $p.Threads)
    }
    Log "[dry run] no measurements taken."
    return
}

# --------------------------------------------------------------------------
# Measurement loop
# --------------------------------------------------------------------------
$hostname = $env:COMPUTERNAME
$dateStamp = (Get-Date).ToString("yyyy-MM-ddTHH:mm:ss")
$results = @()

foreach ($p in $Plan) {
    Log ("[run] {0}  N={1}  T={2}  warmup={3}  measured={4}" -f $p.Version, $p.N, $p.Threads, $WarmupRuns, $MeasuredRuns)
    $dump = Join-Path $DumpDir ("dump_{0}_N{1}.txt" -f $p.Version, $p.N)

    for ($w = 1; $w -le $WarmupRuns; $w++) {
        if ($p.Version -eq "v5_cuda") { [void](Run-V5 -N $p.N -DumpFile $dump) }
        else                          { [void](Run-V4 -N $p.N -T $CompareThreads -DumpFile $dump) }
    }
    for ($r = 1; $r -le $MeasuredRuns; $r++) {
        if ($p.Version -eq "v5_cuda") {
            $res = Run-V5 -N $p.N -DumpFile $dump
        } else {
            $res = Run-V4 -N $p.N -T $CompareThreads -DumpFile $dump
        }
        Log ("  run {0}/{1}: {2:N3} ms" -f $r, $MeasuredRuns, $res.TimeMs)
        $results += [PSCustomObject]@{
            version       = $p.Version
            run_id        = $r
            hostname      = $hostname
            date          = $dateStamp
            N             = [UInt64]$p.N
            threads_label = "$($p.Threads)"
            time_ms       = [Math]::Round($res.TimeMs, 6)
            checksum      = $res.Checksum
        }
    }
}

# --------------------------------------------------------------------------
# Aggregate
# --------------------------------------------------------------------------
$resultsCsv = Join-Path $WorkDir "bench_cuda_results.csv"
$results | Sort-Object N, version, run_id |
    Export-Csv -Path $resultsCsv -NoTypeInformation -Encoding UTF8
Log "[csv] $resultsCsv"

$agg = $results |
    Group-Object @{Expression="version"}, @{Expression="N"} |
    ForEach-Object {
        $g = $_.Group
        $first = $g[0]
        $avg = ($g | Measure-Object -Property time_ms -Average).Average
        $min = ($g | Measure-Object -Property time_ms -Minimum).Minimum
        $max = ($g | Measure-Object -Property time_ms -Maximum).Maximum
        $tput = ([double]$first.N) / ($avg / 1000.0) / 1.0e6
        [PSCustomObject]@{
            version            = $first.version
            N                  = $first.N
            threads_label      = $first.threads_label
            avg_ms             = [Math]::Round($avg, 4)
            min_ms             = [Math]::Round($min, 4)
            max_ms             = [Math]::Round($max, 4)
            throughput_Mn_s    = [Math]::Round($tput, 3)
            v5_speedup_vs_v4   = 0.0
        }
    }

# fill in v5_speedup_vs_v4
foreach ($n in $Nset) {
    $r5 = $agg | Where-Object { $_.version -eq "v5_cuda" -and $_.N -eq $n }
    $r4 = $agg | Where-Object { $_.version -eq "v4_cpu"  -and $_.N -eq $n }
    if ($r5 -and $r4 -and $r5.avg_ms -gt 0) {
        $r5.v5_speedup_vs_v4 = [Math]::Round([double]$r4.avg_ms / [double]$r5.avg_ms, 4)
    }
}

$summary = $agg | Sort-Object N, version
$summaryCsv = Join-Path $WorkDir "bench_cuda_summary.csv"
$summary | Export-Csv -Path $summaryCsv -NoTypeInformation -Encoding UTF8
Log "[csv] $summaryCsv"

# --------------------------------------------------------------------------
# Console summary
# --------------------------------------------------------------------------
""
Write-Host "=================== bench_cuda summary ==================="
foreach ($line in $v5_stderr) { Write-Host $line }
$summary | Format-Table version, N, threads_label, avg_ms, min_ms, throughput_Mn_s, v5_speedup_vs_v4 -AutoSize

if ($cmpAvail) {
    Write-Host ""
    Write-Host "Pairwise GPU vs CPU (v4 @ T=$CompareThreads):"
    foreach ($n in $Nset) {
        $r4 = $agg | Where-Object { $_.version -eq "v4_cpu"  -and $_.N -eq $n }
        $r5 = $agg | Where-Object { $_.version -eq "v5_cuda" -and $_.N -eq $n }
        if ($r4 -and $r5) {
            $ratio = [double]$r4.avg_ms / [double]$r5.avg_ms
            Write-Host ("  N={0,-12}  v4(CPU,T={1})={2,8:N3} ms  v5(GPU)={3,8:N3} ms  GPU speedup={4:N3}x" -f $n, $CompareThreads, $r4.avg_ms, $r5.avg_ms, $ratio)
        }
    }
}

Write-Host ""
Write-Host "Files in: $WorkDir"
Log "=== bench_cuda.ps1 finished ==="
