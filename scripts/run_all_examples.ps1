param(
    [string]$Gpp = "C:\msys64\ucrt64\bin\g++.exe",
    [switch]$SkipCuda,
    [switch]$SkipStandalone,
    [switch]$SkipSeird,
    [switch]$SkipModel1Rng16,
    [switch]$SkipOldArch,
    [switch]$KeepOutputs
)

$ErrorActionPreference = "Stop"

$bundleDir = Split-Path $PSScriptRoot -Parent
$standaloneDir = Join-Path $bundleDir "standalone"
$seirdDir = Join-Path $bundleDir "seird"
$outDir = Join-Path $bundleDir "example_runs"

if ((Test-Path $outDir) -and (-not $KeepOutputs)) {
    Remove-Item $outDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

function Log([string]$msg) {
    $ts = (Get-Date).ToString("yyyy-MM-dd HH:mm:ss")
    Write-Host "[$ts] $msg"
}

function Assert-Exists([string]$path) {
    if (-not (Test-Path $path)) {
        throw "Файл не найден: $path"
    }
}

function Run-NativeInDir {
    param(
        [string]$Exe,
        [string[]]$ArgList,
        [string]$WorkDir
    )

    Push-Location $WorkDir
    try {
        & $Exe @ArgList
        if ($LASTEXITCODE -ne 0) {
            throw "Команда завершилась ошибкой: $Exe $($ArgList -join ' ')"
        }
    }
    finally {
        Pop-Location
    }
}

function Find-Nvcc {
    $cmd = Get-Command nvcc.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    if ($env:CUDA_PATH) {
        $candidate = Join-Path $env:CUDA_PATH "bin\nvcc.exe"
        if (Test-Path $candidate) { return $candidate }
    }
    return $null
}

Log "Bundle dir: $bundleDir"
Log "Output dir: $outDir"

if (-not $SkipStandalone) {
    Log "Step 1/5: build standalone v1-v4"
    $buildStandalone = Join-Path $PSScriptRoot "build_standalone_v1_v4.ps1"
    Assert-Exists $buildStandalone
    powershell -ExecutionPolicy Bypass -File $buildStandalone -Gpp $Gpp

    Log "Step 2/5: run standalone examples"
    $standaloneOut = Join-Path $outDir "standalone"
    New-Item -ItemType Directory -Force -Path $standaloneOut | Out-Null

    Run-NativeInDir -Exe (Join-Path $standaloneDir "rng_seq.exe") `
        -ArgList @("100000", (Join-Path $standaloneOut "v1_seq_dump.txt")) `
        -WorkDir $standaloneOut

    Run-NativeInDir -Exe (Join-Path $standaloneDir "rng_par.exe") `
        -ArgList @("100000", (Join-Path $standaloneOut "v2_par_dump.txt"), "8") `
        -WorkDir $standaloneOut

    Run-NativeInDir -Exe (Join-Path $standaloneDir "rng_par_v3.exe") `
        -ArgList @("100000", (Join-Path $standaloneOut "v3_par_dump.txt"), "8") `
        -WorkDir $standaloneOut

    Run-NativeInDir -Exe (Join-Path $standaloneDir "rng_par_v4.exe") `
        -ArgList @("100000", (Join-Path $standaloneOut "v4_par_dump.txt"), "8") `
        -WorkDir $standaloneOut

    if (Test-Path (Join-Path $standaloneDir "verify.cpp")) {
        Log "Step 2a: build and run standalone verify"
        & $Gpp -O3 -std=c++17 (Join-Path $standaloneDir "verify.cpp") -o (Join-Path $standaloneDir "verify.exe")
        if ($LASTEXITCODE -eq 0) {
            Run-NativeInDir -Exe (Join-Path $standaloneDir "verify.exe") -ArgList @() -WorkDir $standaloneOut
        }
    }
}

if (-not $SkipSeird) {
    Log "Step 3/5: build SEIRD v1-v4"
    $buildV1 = Join-Path $PSScriptRoot "build_seird_strict_v1.ps1"
    $buildV34 = Join-Path $PSScriptRoot "build_seird_strict_v34.ps1"
    powershell -ExecutionPolicy Bypass -File $buildV1 -Gpp $Gpp
    powershell -ExecutionPolicy Bypass -File $buildV34 -Gpp $Gpp

    Log "Step 4/5: run basic SEIRD examples"
    $seirdOut = Join-Path $outDir "seird_basic"
    New-Item -ItemType Directory -Force -Path $seirdOut | Out-Null
    $benchOpenmp = Join-Path $PSScriptRoot "bench_openmp.ps1"

    foreach ($core in @("v1", "v2", "v3", "v4")) {
        $exe = Join-Path $seirdDir ("seird_{0}_STRICT.exe" -f $core)
        if ($core -eq "v1") { $exe = Join-Path $seirdDir "seird_v1_STRICT.exe" }
        $csv = Join-Path $seirdOut ("{0}_threads_1_16.csv" -f $core)
        powershell -ExecutionPolicy Bypass -File $benchOpenmp `
            -Exe $exe `
            -Nr 20 `
            -Tmod 30 `
            -Block 100000 `
            -ThreadsList "1,16" `
            -WarmupRuns 0 `
            -MeasuredRuns 1 `
            -CsvPath $csv
    }
}

if (-not $SkipModel1Rng16) {
    Log "Step 5/5: run model=1 thread, rng=16 threads for v1-v4"
    $runModel1 = Join-Path $PSScriptRoot "run_model1_rng16_v1_v4.ps1"
    powershell -ExecutionPolicy Bypass -File $runModel1 `
        -Nr 20 `
        -Tmod 30 `
        -Block 100000 `
        -RngThreads 16 `
        -RngPrefetch 8192 `
        -WarmupRuns 0 `
        -MeasuredRuns 1
}

if (-not $SkipOldArch) {
    Log "Extra step: run old-architecture v1-v4 comparison"
    $benchOld = Join-Path $PSScriptRoot "bench_old_arch_rngcore.ps1"
    $oldExe = Join-Path $seirdDir "old_profiled_rngcore_compare.exe"
    if (Test-Path $oldExe) {
        powershell -ExecutionPolicy Bypass -File $benchOld `
            -Exe $oldExe `
            -Cores "v1,v2,v3,v4" `
            -Nr 20 `
            -Tmod 30 `
            -WarmupRuns 0 `
            -MeasuredRuns 1 `
            -CsvPath (Join-Path $outDir "old_arch_v1_v4.csv")
    } else {
        Log "Skip old-arch example: executable not found ($oldExe)"
    }
}

if (-not $SkipCuda) {
    $nvcc = Find-Nvcc
    if ($nvcc) {
        Log "CUDA detected: run v5 example"
        $benchCuda = Join-Path $PSScriptRoot "bench_cuda.ps1"
        try {
            powershell -ExecutionPolicy Bypass -File $benchCuda `
                -Nvcc $nvcc `
                -NList "1000000" `
                -WarmupRuns 0 `
                -MeasuredRuns 1 `
                -WorkDir (Join-Path $outDir "cuda_v5")
        }
        catch {
            Log "CUDA v5 step skipped after build/runtime failure."
            Log "Reason: $($_.Exception.Message)"
            Log "Typical fix on this machine: CUDA Toolkit 12.4+ or an older MSVC toolset compatible with CUDA 12.2."
        }
    } else {
        Log "CUDA not found; v5 example skipped"
    }
}

Log "Готово. Outputs saved in: $outDir"
