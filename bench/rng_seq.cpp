// rng_seq.cpp — последовательная (однопоточная) реализация ГПСЧ.
//
// Запуск:   rng_seq <N> <output_file>
//   N            — сколько чисел сгенерировать
//   output_file  — куда записать первые min(N, 100) значений (для проверки)
//
// На stdout печатается: N, время в мс, контрольная сумма.

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <chrono>
#include "rng128.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: %s <N> <output_file>\n", argv[0]);
        return 1;
    }
    uint64_t N = std::strtoull(argv[1], nullptr, 10);
    const char* out_path = argv[2];

    U128 A = build_A_from_m(RNG_M);
    U128 u = initial_u();

    const uint64_t to_dump = (N < 100ULL) ? N : 100ULL;

    FILE* f = std::fopen(out_path, "w");
    if (!f) { std::printf("cannot open %s\n", out_path); return 2; }
    std::fprintf(f, "# Sequential 128-bit congruential RNG (PARMONC, rnd128_)\n");
    std::fprintf(f, "# u_0 = 1, u_n = u_{n-1} * A (mod 2^128), alpha = u * 2^-128\n");
    std::fprintf(f, "# First %llu of %llu generated\n",
                 (unsigned long long)to_dump, (unsigned long long)N);
    std::fprintf(f, "#---------------------------------------------------------\n");

    auto t0 = std::chrono::high_resolution_clock::now();

    double checksum = 0.0;
    for (uint64_t i = 0; i < N; ++i) {
        double a = rng_next(u, A);
        checksum += a;
        if (i < to_dump) std::fprintf(f, "%6llu\t%.15f\n", (unsigned long long)i, a);
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::fclose(f);
    std::printf("SEQ N=%llu time_ms=%.3f checksum=%.6f\n",
                (unsigned long long)N, ms, checksum);
    return 0;
}
