// rng_baseline.cpp — ИСХОДНАЯ (неоптимизированная) последовательная версия.
//
// Здесь rnd128_() взят дословно из poison_incub_aka_SEIRD_2.cpp без единого
// изменения. Это эталон "до оптимизации": с массивом int u[10] по основанию
// 2^13 и распаковкой через double-таблицу x[].
//
// Задача файла — в той же измерительной обвязке (chrono, тот же формат
// stdout, тот же дамп первых 100 чисел), что и rng_seq.cpp / rng_par.cpp,
// чтобы бенчмарк сравнивал именно изменения в арифметике, а не разницу
// в способе замера.
//
// Запуск:   rng_baseline <N> <output_file>

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <chrono>

// === НАЧАЛО ДОСЛОВНОЙ КОПИИ ИЗ poison_incub_aka_SEIRD_2.cpp ====================
// (комментарии оригинала сохранены)

// 128-bit pseudorandom number generator: returns alpha ~ U(0, 1)
static double rnd128_()
{
    static int u[10] = { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    const int m[10] = { 1941, 1821, 3812, 1310, 68, 2906, 2335, 2609, 6859, 1999 };
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
    int n, c0, c1, c2, c3, c4, c5, c6, c7, c8, c9;

    c0 = m[0] * u[0];
    c1 = m[0] * u[1] + m[1] * u[0];
    c2 = m[0] * u[2] + m[1] * u[1] + m[2] * u[0];
    c3 = m[0] * u[3] + m[1] * u[2] + m[2] * u[1] + m[3] * u[0];
    c4 = m[0] * u[4] + m[1] * u[3] + m[2] * u[2] + m[3] * u[1] + m[4] * u[0];
    c5 = m[0] * u[5] + m[1] * u[4] + m[2] * u[3] + m[3] * u[2] + m[4] * u[1] + m[5] * u[0];
    c6 = m[0] * u[6] + m[1] * u[5] + m[2] * u[4] + m[3] * u[3] + m[4] * u[2] + m[5] * u[1]
        + m[6] * u[0];
    c7 = m[0] * u[7] + m[1] * u[6] + m[2] * u[5] + m[3] * u[4] + m[4] * u[3] + m[5] * u[2]
        + m[6] * u[1] + m[7] * u[0];
    c8 = m[0] * u[8] + m[1] * u[7] + m[2] * u[6] + m[3] * u[5] + m[4] * u[4] + m[5] * u[3]
        + m[6] * u[2] + m[7] * u[1] + m[8] * u[0];
    c9 = m[0] * u[9] + m[1] * u[8] + m[2] * u[7] + m[3] * u[6] + m[4] * u[5] + m[5] * u[4]
        + m[6] * u[3] + m[7] * u[2] + m[8] * u[1] + m[9] * u[0];

    u[0] = c0 - ((c0 >> 13) << 13);
    n = c1 + (c0 >> 13);
    u[1] = n - ((n >> 13) << 13);
    n = c2 + (n >> 13);
    u[2] = n - ((n >> 13) << 13);
    n = c3 + (n >> 13);
    u[3] = n - ((n >> 13) << 13);
    n = c4 + (n >> 13);
    u[4] = n - ((n >> 13) << 13);
    n = c5 + (n >> 13);
    u[5] = n - ((n >> 13) << 13);
    n = c6 + (n >> 13);
    u[6] = n - ((n >> 13) << 13);
    n = c7 + (n >> 13);
    u[7] = n - ((n >> 13) << 13);
    n = c8 + (n >> 13);
    u[8] = n - ((n >> 13) << 13);
    n = c9 + (n >> 13);
    u[9] = n - ((n >> 11) << 11);

    return u[0] * x[0] + u[1] * x[1] + u[2] * x[2] + u[3] * x[3] + u[4] * x[4]
         + u[5] * x[5] + u[6] * x[6] + u[7] * x[7] + u[8] * x[8] + u[9] * x[9];
}
// === КОНЕЦ ДОСЛОВНОЙ КОПИИ ====================================================

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: %s <N> <output_file>\n", argv[0]);
        return 1;
    }
    uint64_t N = std::strtoull(argv[1], nullptr, 10);
    const char* out_path = argv[2];

    const uint64_t to_dump = (N < 100ULL) ? N : 100ULL;

    FILE* f = std::fopen(out_path, "w");
    if (!f) { std::printf("cannot open %s\n", out_path); return 2; }
    std::fprintf(f, "# Baseline (original) rnd128_ — копия 1:1 из poison_incub_aka_SEIRD_2.cpp\n");
    std::fprintf(f, "# u[10] base 2^13, double-table unpack\n");
    std::fprintf(f, "# First %llu of %llu generated\n",
                 (unsigned long long)to_dump, (unsigned long long)N);
    std::fprintf(f, "#---------------------------------------------------------\n");

    auto t0 = std::chrono::high_resolution_clock::now();

    double checksum = 0.0;
    for (uint64_t i = 0; i < N; ++i) {
        double a = rnd128_();
        checksum += a;
        if (i < to_dump) std::fprintf(f, "%6llu\t%.15f\n", (unsigned long long)i, a);
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::fclose(f);
    std::printf("BASE N=%llu time_ms=%.3f checksum=%.6f\n",
                (unsigned long long)N, ms, checksum);
    return 0;
}
