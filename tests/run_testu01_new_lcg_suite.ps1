param(
    [string]$MatrixScript = "$PSScriptRoot\run_testu01_matrix.ps1",
    [string]$OutRoot      = "$PSScriptRoot\testu01_new_lcg_20260522",
    [int]$Threads         = 20,
    [int]$Chunk           = 4194304,
    [string]$Generators   = "sprng_lcg64,mkl_mcg59_like,mrg32k3a,lehmer127_mersenne"
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path $MatrixScript)) {
    throw "Сценарий матрицы не найден: $MatrixScript"
}

if (-not (Test-Path $OutRoot)) {
    New-Item -ItemType Directory -Path $OutRoot | Out-Null
}

function Invoke-Battery {
    param(
        [string]$Battery,
        [string]$OutDir
    )
    if (-not (Test-Path $OutDir)) {
        New-Item -ItemType Directory -Path $OutDir | Out-Null
    }
    Write-Host "=== Батарея: $Battery -> $OutDir ==="
    & powershell -ExecutionPolicy Bypass -File $MatrixScript `
        -Battery $Battery `
        -Threads $Threads `
        -Chunk $Chunk `
        -Generators $Generators `
        -OutDir $OutDir

    if ($LASTEXITCODE -ne 0) {
        throw "Battery '$Battery' завершилась ошибкой с кодом $LASTEXITCODE"
    }
}

Invoke-Battery -Battery "small" -OutDir (Join-Path $OutRoot "small")
Invoke-Battery -Battery "crush" -OutDir (Join-Path $OutRoot "crush")

# В некоторых сборках третья батарея называется "big", в других — "bigcrush".
try {
    Invoke-Battery -Battery "big" -OutDir (Join-Path $OutRoot "big")
}
catch {
    Write-Host "Батарея 'big' завершилась ошибкой, повторяем как 'bigcrush'..."
    Invoke-Battery -Battery "bigcrush" -OutDir (Join-Path $OutRoot "big")
}

Write-Host ""
Write-Host "Готово. Results root:"
Write-Host "  $OutRoot"
