// rng_par_v3.cpp -- автономный бенчмарк ГПСЧ с "быстрым прыжком"
// извлечённого из seird_hybrid_mpi_omp.cpp (Rng128 + pow_mod_2_128).
//
// Тот же алгоритм горячего цикла, что и в rng_par.cpp: каждый поток t проходит свой
// фрагмент решётки u_n = u_0 * A^n (mod 2^128), начиная с состояния
//   u_start[t] = u_0 * A^(t * mu)   (mu = N / T).
//
// Единственное принципиальное отличие от rng_par.cpp, которое демонстрирует этот файл,
// заключается в умножении 128x128 -> младшие 128 бит: здесь используются
// нативные intrinsics (_umul128 в MSVC, unsigned __int128 в GCC/Clang) вместо
// ручного варианта 32x32 из rng128.h. Фаза инициализации (построение
// u_start[T]) is now organized identically to rng_par.cpp so that the
// pairwise comparison v2 vs v3 is a clean intrinsic-vs-manual test.
//
// Стоимость инициализации: log2(mu) + (T-1) умножений, все на главном
// потоке до входа в параллельную область. Предыдущий вариант делал T
// independent pow_mod_2_128(A, t * mu) calls in parallel inside the
// region; the new variant trades a tiny bit of serial work for a
// shared u_start table, which is the standard PARMONC layout.
//
// Все три бинарных файла бенчмарка (baseline, rng_par.cpp и этот файл) порождают
// the same 128-bit sequence and therefore the same checksum -- verified
// by bench_rng3.ps1 sanity check at startup.
//
// Запуск:  rng_par_v3 <N> <output_file> [num_threads]
// В stdout: PAR3 N=<N> T=<T> time_ms=<ms> checksum=<sum>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <omp.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

struct U128 {
    uint64_t lo;
    uint64_t hi;
    U128(uint64_t lo_ = 0, uint64_t hi_ = 0) : lo(lo_), hi(hi_) {}
};

static inline U128 mul_mod_2_128(const U128& a, const U128& b) {
#if defined(_MSC_VER)
    uint64_t p1_hi = 0;
    const uint64_t p1_lo = _umul128(a.lo, b.lo, &p1_hi);
    const uint64_t cross1 = a.lo * b.hi;
    const uint64_t cross2 = a.hi * b.lo;
    return U128(p1_lo, p1_hi + cross1 + cross2);
#else
    using u128 = unsigned __int128;
    const u128 p00 = (u128)a.lo * (u128)b.lo;
    const u128 cross = (u128)a.lo * (u128)b.hi + (u128)a.hi * (u128)b.lo;
    const uint64_t lo = (uint64_t)p00;
    const uint64_t hi = (uint64_t)((p00 >> 64) + cross);
    return U128(lo, hi);
#endif
}

static inline U128 add_shifted_small(U128 cur, uint64_t v, int shift) {
    if (shift < 64) {
        const uint64_t add_lo = v << shift;
        cur.lo += add_lo;
        if (cur.lo < add_lo) ++cur.hi;
        if (shift != 0) cur.hi += (v >> (64 - shift));
    } else {
        cur.hi += (v << (shift - 64));
    }
    return cur;
}

static const int RNG_M[10] = {1941, 1821, 3812, 1310, 68, 2906, 2335, 2609, 6859, 1999};

static U128 build_A() {
    static const int shifts[10] = {0, 13, 26, 39, 52, 65, 78, 91, 104, 117};
    U128 a;
    for (int i = 0; i < 10; ++i) {
        a = add_shifted_small(a, (uint64_t)RNG_M[i], shifts[i]);
    }
    return a;
}

static U128 pow_mod_2_128(U128 base, uint64_t exp) {
    U128 result(1, 0);
    while (exp != 0) {
        if (exp & 1ULL) result = mul_mod_2_128(result, base);
        base = mul_mod_2_128(base, base);
        exp >>= 1;
    }
    return result;
}

static inline double u_to_double(U128 u) {
    const uint64_t mantissa53 = u.hi >> 11;
    return (double)mantissa53 * (1.0 / 9007199254740992.0); // 2^-53
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: %s <N> <output_file> [num_threads]\n", argv[0]);
        return 1;
    }
    const uint64_t N = std::strtoull(argv[1], nullptr, 10);
    const char* out_path = argv[2];
    int T = (argc >= 4) ? std::atoi(argv[3]) : omp_get_max_threads();
    if (T < 1) T = 1;
    omp_set_num_threads(T);

    const U128 A = build_A();
    const U128 u0(1, 0);
    const uint64_t mu = N / (uint64_t)T;

    std::vector<double> buf(N);

    auto t0 = std::chrono::high_resolution_clock::now();

    // ------------------------------------------------------------------
    // Build u_start[T] once, on the master thread, before the parallel
    // region. u_start[t] = u_0 * A^(t * mu).
    //
    // Compute A_mu = A^mu via binary exponentiation -- this is the only
    // pow_mod_2_128 call. Then iterate u_start[t] = u_start[t-1] * A_mu.
    // Total cost: ~log2(mu) + (T-1) multiplications, all serial.
    //
    // Equivalent to the layout in rng_par.cpp; the parallel region below
    // simply reads u_start[tid] and proceeds with the hot loop.
    // ------------------------------------------------------------------
    std::vector<U128> u_start(T);
    u_start[0] = u0;
    if (T > 1) {
        const U128 A_mu = pow_mod_2_128(A, mu);
        for (int t = 1; t < T; ++t) {
            u_start[t] = mul_mod_2_128(u_start[t - 1], A_mu);
        }
    }

    double checksum = 0.0;
    #pragma omp parallel reduction(+:checksum) num_threads(T)
    {
        const int tid = omp_get_thread_num();
        const uint64_t start = (uint64_t)tid * mu;
        const uint64_t end   = (tid == T - 1) ? N : (start + mu);

        U128 u = u_start[tid];

        double local_sum = 0.0;
        for (uint64_t i = start; i < end; ++i) {
            u = mul_mod_2_128(u, A);
            const double a = u_to_double(u);
            buf[i] = a;
            local_sum += a;
        }
        checksum += local_sum;
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    const uint64_t to_dump = (N < 100ULL) ? N : 100ULL;
    FILE* f = std::fopen(out_path, "w");
    if (!f) { std::printf("cannot open %s\n", out_path); return 2; }
    std::fprintf(f, "# Parallel 128-bit MCG (Rng128 from seird_hybrid_mpi_omp.cpp,"
                    " native _umul128 / __int128 + precomputed u_start[T])\n");
    std::fprintf(f, "# threads=%d, mu=%llu (block per thread)\n",
                 T, (unsigned long long)mu);
    std::fprintf(f, "# First %llu of %llu generated\n",
                 (unsigned long long)to_dump, (unsigned long long)N);
    std::fprintf(f, "#---------------------------------------------------------\n");
    for (uint64_t i = 0; i < to_dump; ++i) {
        std::fprintf(f, "%6llu\t%.15f\n",
                     (unsigned long long)i, buf[(size_t)i]);
    }
    std::fclose(f);

    std::printf("PAR3 N=%llu T=%d time_ms=%.3f checksum=%.6f\n",
                (unsigned long long)N, T, ms, checksum);
    return 0;
}
