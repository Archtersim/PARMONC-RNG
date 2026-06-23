param(
    [string]$Gpp = "C:\msys64\ucrt64\bin\g++.exe",
    [string]$SeqSrc = "C:\Users\tersi\Downloads\poison_incub_aka_SEIRD_2.cpp",
    [string]$ParSrc = "C:\Users\tersi\Documents\Codex\2026-04-24\files-mentioned-by-the-user-poison\seird_hybrid_mpi_omp.cpp",
    [string]$WorkDir = "C:\Users\tersi\Documents\Codex\2026-04-24\files-mentioned-by-the-user-poison",
    [int]$Nr = 100,
    [int]$Tmod = 90,
    [int]$Threads = 8,
    [UInt64]$Block = 1000000,
    [int]$RngCount = 100000
)

$ErrorActionPreference = "Stop"
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
Set-Location $WorkDir

function Ensure-Ok($code, $msg) {
    if ($code -ne 0) { throw $msg }
}

Write-Host "1) Сборка основных исполняемых файлов..."
& $Gpp -O3 $SeqSrc -o "$WorkDir\seq.exe"
Ensure-Ok $LASTEXITCODE "Сборка завершилась ошибкой: seq.exe"
& $Gpp -O3 -fopenmp $ParSrc -o "$WorkDir\par.exe"
Ensure-Ok $LASTEXITCODE "Сборка завершилась ошибкой: par.exe"

Write-Host "2) Сборка вспомогательной программы для дампа RNG (seq)..."
$seqDumpCpp = @'
#include <cstdio>
#include <cstdlib>
#include <cmath>

double rnd128_() {
    static int u[10] = { 1,0,0,0,0,0,0,0,0,0 };
    const int m[10] = { 1941,1821,3812,1310,68,2906,2335,2609,6859,1999 };
    const double x[10] = {
        0.00000000000000000000000000000000000000293873587705571880,
        0.00000000000000000000000000000000002407412430484044800000,
        0.00000000000000000000000000000019721522630525295000000000,
        0.00000000000000000000000000161558713389263220000000000000,
        0.00000000000000000000001323488980084844300000000000000000,
        0.00000000000000000010842021724855044000000000000000000000,
        0.00000000000000088817841970012523000000000000000000000000,
        0.00000000000727595761418342590000000000000000000000000000,
        0.00000005960464477539062500000000000000000000000000000000,
        0.00048828125000000000000000000000000000000000000000000000 };
    int n,c0,c1,c2,c3,c4,c5,c6,c7,c8,c9;
    c0 = m[0]*u[0];
    c1 = m[0]*u[1] + m[1]*u[0];
    c2 = m[0]*u[2] + m[1]*u[1] + m[2]*u[0];
    c3 = m[0]*u[3] + m[1]*u[2] + m[2]*u[1] + m[3]*u[0];
    c4 = m[0]*u[4] + m[1]*u[3] + m[2]*u[2] + m[3]*u[1] + m[4]*u[0];
    c5 = m[0]*u[5] + m[1]*u[4] + m[2]*u[3] + m[3]*u[2] + m[4]*u[1] + m[5]*u[0];
    c6 = m[0]*u[6] + m[1]*u[5] + m[2]*u[4] + m[3]*u[3] + m[4]*u[2] + m[5]*u[1] + m[6]*u[0];
    c7 = m[0]*u[7] + m[1]*u[6] + m[2]*u[5] + m[3]*u[4] + m[4]*u[3] + m[5]*u[2] + m[6]*u[1] + m[7]*u[0];
    c8 = m[0]*u[8] + m[1]*u[7] + m[2]*u[6] + m[3]*u[5] + m[4]*u[4] + m[5]*u[3] + m[6]*u[2] + m[7]*u[1] + m[8]*u[0];
    c9 = m[0]*u[9] + m[1]*u[8] + m[2]*u[7] + m[3]*u[6] + m[4]*u[5] + m[5]*u[4] + m[6]*u[3] + m[7]*u[2] + m[8]*u[1] + m[9]*u[0];
    u[0] = c0 - ((c0 >> 13) << 13);
    n = c1 + (c0 >> 13); u[1] = n - ((n >> 13) << 13);
    n = c2 + (n >> 13); u[2] = n - ((n >> 13) << 13);
    n = c3 + (n >> 13); u[3] = n - ((n >> 13) << 13);
    n = c4 + (n >> 13); u[4] = n - ((n >> 13) << 13);
    n = c5 + (n >> 13); u[5] = n - ((n >> 13) << 13);
    n = c6 + (n >> 13); u[6] = n - ((n >> 13) << 13);
    n = c7 + (n >> 13); u[7] = n - ((n >> 13) << 13);
    n = c8 + (n >> 13); u[8] = n - ((n >> 13) << 13);
    n = c9 + (n >> 13); u[9] = n - ((n >> 11) << 11);
    return u[0]*x[0] + u[1]*x[1] + u[2]*x[2] + u[3]*x[3] + u[4]*x[4] + u[5]*x[5] + u[6]*x[6] + u[7]*x[7] + u[8]*x[8] + u[9]*x[9];
}

int main(int argc, char** argv) {
    int n = 1000;
    if (argc > 1) n = std::atoi(argv[1]);
    FILE* f = std::fopen("rng_seq_numbers.txt", "w");
    for (int i=0;i<n;i++) std::fprintf(f, "%.17g\n", rnd128_());
    std::fclose(f);
    return 0;
}
'@
$seqDumpCpp | Set-Content "$WorkDir\rng_dump_seq.cpp" -Encoding ASCII
& $Gpp -O3 "$WorkDir\rng_dump_seq.cpp" -o "$WorkDir\rng_dump_seq.exe"
Ensure-Ok $LASTEXITCODE "Сборка завершилась ошибкой: rng_dump_seq.exe"

Write-Host "3) Сборка вспомогательной программы для дампа RNG (par RNG core)..."
$parDumpCpp = @'
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <limits>

using u128 = unsigned __int128;
struct Rng128 {
    u128 state;
    static constexpr int M[10] = {1941,1821,3812,1310,68,2906,2335,2609,6859,1999};
    static constexpr u128 multiplier() {
        constexpr int shifts[10] = {0,13,26,39,52,65,78,91,104,117};
        u128 a = 0;
        for (int i=0;i<10;i++) a |= (u128)M[i] << shifts[i];
        return a;
    }
    static const u128 A;
    explicit Rng128(u128 s=1): state(s==0?1:s) {}
    double next_u01() {
        state = state * A;
        uint64_t hi = (uint64_t)(state >> 64);
        uint64_t m53 = hi >> 11;
        double x = (double)m53 * (1.0 / 9007199254740992.0);
        if (x <= 0.0) x = std::numeric_limits<double>::min();
        if (x >= 1.0) x = std::nextafter(1.0, 0.0);
        return x;
    }
};
const u128 Rng128::A = Rng128::multiplier();

int main(int argc, char** argv) {
    int n = 1000;
    if (argc > 1) n = std::atoi(argv[1]);
    Rng128 r(1);
    FILE* f = std::fopen("rng_par_numbers.txt", "w");
    for (int i=0;i<n;i++) std::fprintf(f, "%.17g\n", r.next_u01());
    std::fclose(f);
    return 0;
}
'@
$parDumpCpp | Set-Content "$WorkDir\rng_dump_par.cpp" -Encoding ASCII
& $Gpp -O3 "$WorkDir\rng_dump_par.cpp" -o "$WorkDir\rng_dump_par.exe"
Ensure-Ok $LASTEXITCODE "Сборка завершилась ошибкой: rng_dump_par.exe"

Write-Host "4) Запуск основных алгоритмов и сбор времен..."
$rows = @()
$stamp = (Get-Date).ToString("yyyy-MM-ddTHH:mm:ss")

$sw = [Diagnostics.Stopwatch]::StartNew()
& "$WorkDir\seq.exe" > "$WorkDir\seq_run.log"
$sw.Stop()
Ensure-Ok $LASTEXITCODE "Run failed: seq.exe"
Copy-Item "$WorkDir\Matogf.txt" "$WorkDir\Matogf_seq.txt" -Force
Copy-Item "$WorkDir\stat_errorf.txt" "$WorkDir\stat_errorf_seq.txt" -Force
Copy-Item "$WorkDir\time_elapsed.txt" "$WorkDir\time_elapsed_seq.txt" -Force
$rows += [pscustomobject]@{
    mode = "seq_old"
    date = $stamp
    Nr = 100
    Tmod = 90
    threads = 1
    wall_time_s = [Math]::Round($sw.Elapsed.TotalSeconds, 6)
}

$sw.Restart()
& "$WorkDir\par.exe" --Nr $Nr --Tmod $Tmod --threads $Threads --block $Block > "$WorkDir\par_run.log"
$sw.Stop()
Ensure-Ok $LASTEXITCODE "Run failed: par.exe"
Copy-Item "$WorkDir\Matogf.txt" "$WorkDir\Matogf_par.txt" -Force
Copy-Item "$WorkDir\stat_errorf.txt" "$WorkDir\stat_errorf_par.txt" -Force
Copy-Item "$WorkDir\time_elapsed.txt" "$WorkDir\time_elapsed_par.txt" -Force
$rows += [pscustomobject]@{
    mode = "par_new"
    date = $stamp
    Nr = $Nr
    Tmod = $Tmod
    threads = $Threads
    wall_time_s = [Math]::Round($sw.Elapsed.TotalSeconds, 6)
}

$rows | Export-Csv "$WorkDir\timing_table.csv" -NoTypeInformation -Encoding UTF8

Write-Host "5) Выгрузка потоков чисел RNG..."
& "$WorkDir\rng_dump_seq.exe" $RngCount
Ensure-Ok $LASTEXITCODE "Run failed: rng_dump_seq.exe"
& "$WorkDir\rng_dump_par.exe" $RngCount
Ensure-Ok $LASTEXITCODE "Run failed: rng_dump_par.exe"

Write-Host "Готово."
Write-Host "Файлы:"
Write-Host "  $WorkDir\\rng_seq_numbers.txt"
Write-Host "  $WorkDir\\rng_par_numbers.txt"
Write-Host "  $WorkDir\\timing_table.csv"
Write-Host "  $WorkDir\\Matogf_seq.txt / Matogf_par.txt"
Write-Host "  $WorkDir\\stat_errorf_seq.txt / stat_errorf_par.txt"
