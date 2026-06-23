// rng_par_v4.cpp -- ISA-ориентированная оптимизация 128-битного MCG PARMONC.
//
// Порождается та же числовая последовательность
//   u_n = u_0 * A^n (mod 2^128),
// что и в v1/v2/v3, но горячий цикл переписан так, чтобы использовать
// инструкции, реально доступные на Intel Core Ultra 7 (Arrow Lake):
// AVX2 + BMI2 + FMA3, без AVX-512.
//
// В бинарном файле присутствуют две реализации, выбираемые во время
// выполнения через CPUID:
//
//   путь A: "avx2"   -- тело с 4-way ILP: четыре независимые цепочки
//                       ГПСЧ, продвигаемые шагом A^4. На каждой итерации
//                       получается 4 числа double, которые упаковываются
//                       в один __m256d и записываются одним
//                       _mm256_storeu_pd. Это разгружает OoO-механику
//                       процессора: в полёте находятся 4 независимые
//                       цепочки MUL, а число записей уменьшается вдвое.
//
//   путь B: "scalar" -- горячий цикл v3 (одна цепочка, скалярная запись).
//                       Используется как запасной путь, если CPUID
//                       не сообщает о наличии AVX2.
//
// Почему это работает на данном CPU:
//   * для умножения 64x64 -> 128 (ядро mul_mod_2_128) в AVX2 нет
//     SIMD-эквивалента; в AVX-512 есть VPMULLQ, но только для младших
//     64 бит результата;
//   * поэтому само умножение не SIMD-векторизуется, а выполняется в
//     четырёх независимых цепочках через ILP;
//   * BMI2-инструкция MULX автоматически генерируется gcc при -mbmi2
//     в местах, где используются __umul128 / unsigned __int128;
//   * AVX2 используется только на этапе упаковки и записи результата,
//     а также при преобразовании 53-битной мантиссы в double по 4 сразу.
//
// Численная эквивалентность: получается побитово та же последовательность
// u_n. Контрольная сумма может отличаться от v3 на 1-2 ULP, поскольку
// порядок редукции другой (4 частичные суммы и затем горизонтальное
// сложение), но допуск 1e-3 в bench_rng3.ps1 это покрывает с большим запасом.
//
// Сборка (MSYS2 g++):
//   g++ -O3 -std=c++17 -fopenmp -mavx2 -mbmi2 -mfma rng_par_v4.cpp -o rng_par_v4.exe
// Сборка (MSVC):
//   cl /O2 /EHsc /openmp /arch:AVX2 rng_par_v4.cpp /Fe:rng_par_v4.exe
//
// Запуск:  rng_par_v4 <N> <output_file> [num_threads]
// В stdout: PAR4 N=<N> T=<T> time_ms=<ms> checksum=<sum>
// В stderr: обнаруженные свойства CPU и выбранный путь выполнения

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include <omp.h>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#endif

#include <immintrin.h>

// ---------------------------------------------------------------------
// Определение свойств CPU через CPUID (работает на любой x86_64 ОС)
// ---------------------------------------------------------------------
struct CpuFeatures {
    bool avx   = false;
    bool avx2  = false;
    bool fma   = false;
    bool bmi2  = false;
    bool sse42 = false;
    bool aes   = false;
    bool sha   = false;
    int  max_basic_leaf = 0;

    void detect() {
        unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
#if defined(_MSC_VER)
        int regs[4];
        __cpuid(regs, 0);
        max_basic_leaf = regs[0];
        if (max_basic_leaf >= 1) {
            __cpuid(regs, 1);
            ecx = (unsigned)regs[2];
            edx = (unsigned)regs[3];
            sse42 = (ecx & (1u << 20)) != 0;
            aes   = (ecx & (1u << 25)) != 0;
            avx   = (ecx & (1u << 28)) != 0;
            fma   = (ecx & (1u << 12)) != 0;
        }
        if (max_basic_leaf >= 7) {
            __cpuidex(regs, 7, 0);
            ebx = (unsigned)regs[1];
            avx2 = (ebx & (1u << 5))  != 0;
            bmi2 = (ebx & (1u << 8))  != 0;
            sha  = (ebx & (1u << 29)) != 0;
        }
#else
        __cpuid(0, eax, ebx, ecx, edx);
        max_basic_leaf = (int)eax;
        if (max_basic_leaf >= 1) {
            __cpuid(1, eax, ebx, ecx, edx);
            sse42 = (ecx & (1u << 20)) != 0;
            aes   = (ecx & (1u << 25)) != 0;
            avx   = (ecx & (1u << 28)) != 0;
            fma   = (ecx & (1u << 12)) != 0;
        }
        if (max_basic_leaf >= 7) {
            __cpuid_count(7, 0, eax, ebx, ecx, edx);
            avx2 = (ebx & (1u << 5))  != 0;
            bmi2 = (ebx & (1u << 8))  != 0;
            sha  = (ebx & (1u << 29)) != 0;
        }
#endif
    }
};

// ---------------------------------------------------------------------
// Ядро 128-битного MCG (идентично v3)
// ---------------------------------------------------------------------
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

// ---------------------------------------------------------------------
// Path A (AVX2): 4-way ILP, 256-bit store
// ---------------------------------------------------------------------
// Loop is "write then update":
//   pre-state holds u_init * A^1, A^2, A^3, A^4;
//   iter b writes them into positions [4b, 4b+1, 4b+2, 4b+3]
//   then advances each by A^4.
// Correctness: positions 0..count-1 receive u_init * A^1, A^2, ...,
// which is exactly what the scalar v3 reference loop produces.

#if defined(__GNUC__) && !defined(__INTEL_COMPILER)
__attribute__((target("avx2,fma,bmi2")))
#endif
static double avx2_loop_one_thread(double* dst,
                                   U128 u_init,
                                   const U128& A,
                                   uint64_t count)
{
    if (count == 0) return 0.0;

    // 4 staggered states, holding u_init * A^1, A^2, A^3, A^4.
    U128 u_a = mul_mod_2_128(u_init, A);   // u_init * A^1
    U128 u_b = mul_mod_2_128(u_a,   A);    // u_init * A^2
    U128 u_c = mul_mod_2_128(u_b,   A);    // u_init * A^3
    U128 u_d = mul_mod_2_128(u_c,   A);    // u_init * A^4
    const U128 jump_A4 = pow_mod_2_128(A, 4);

    const __m256d scale = _mm256_set1_pd(1.0 / 9007199254740992.0); // 2^-53
    __m256d sum_vec = _mm256_setzero_pd();

    const uint64_t batches = count >> 2;          // count / 4
    for (uint64_t b = 0; b < batches; ++b) {
        // 1) Extract top 53 bits of each .hi -> mantissa.
        const uint64_t m0 = u_a.hi >> 11;
        const uint64_t m1 = u_b.hi >> 11;
        const uint64_t m2 = u_c.hi >> 11;
        const uint64_t m3 = u_d.hi >> 11;

        // 2) Pack into __m256d, multiply by 2^-53, store with one VMOVUPD.
        const __m256d v = _mm256_setr_pd(
            (double)m0, (double)m1, (double)m2, (double)m3);
        const __m256d r = _mm256_mul_pd(v, scale);
        _mm256_storeu_pd(dst + b * 4, r);
        sum_vec = _mm256_add_pd(sum_vec, r);

        // 3) Advance all four chains by A^4 -- 4 INDEPENDENT mul_mod_2_128
        //    that the OoO engine schedules in parallel.
        u_a = mul_mod_2_128(u_a, jump_A4);
        u_b = mul_mod_2_128(u_b, jump_A4);
        u_c = mul_mod_2_128(u_c, jump_A4);
        u_d = mul_mod_2_128(u_d, jump_A4);
    }

    // Horizontal sum of sum_vec into a scalar.
    __m128d lo  = _mm256_castpd256_pd128(sum_vec);
    __m128d hi  = _mm256_extractf128_pd(sum_vec, 1);
    __m128d s2  = _mm_add_pd(lo, hi);
    __m128d s2h = _mm_unpackhi_pd(s2, s2);
    double sum = _mm_cvtsd_f64(_mm_add_sd(s2, s2h));

    // Tail (1..3 leftover): use the already-advanced u_a, u_b, u_c.
    // After `batches` write-then-update iterations:
    //   u_a holds u_init * A^(4*batches + 1)
    //   u_b holds u_init * A^(4*batches + 2)
    //   u_c holds u_init * A^(4*batches + 3)
    // These are exactly the values needed at positions
    //   [4*batches + 0], [4*batches + 1], [4*batches + 2].
    const U128 tail_us[3] = { u_a, u_b, u_c };
    const uint64_t tail_off = batches * 4;
    for (uint64_t k = 0; tail_off + k < count && k < 3; ++k) {
        const uint64_t m = tail_us[k].hi >> 11;
        const double a = (double)m * (1.0 / 9007199254740992.0);
        dst[tail_off + k] = a;
        sum += a;
    }
    return sum;
}

// ---------------------------------------------------------------------
// Путь B (скалярный запасной вариант) -- горячий цикл v3 без изменений
// ---------------------------------------------------------------------
static double scalar_loop_one_thread(double* dst,
                                     U128 u,
                                     const U128& A,
                                     uint64_t count)
{
    double sum = 0.0;
    for (uint64_t i = 0; i < count; ++i) {
        u = mul_mod_2_128(u, A);
        const uint64_t m = u.hi >> 11;
        const double a = (double)m * (1.0 / 9007199254740992.0);
        dst[i] = a;
        sum += a;
    }
    return sum;
}

// ---------------------------------------------------------------------
// main
// ---------------------------------------------------------------------
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

    // ---- ISA detection (printed to stderr so as not to break stdout
    //                     parsers used by the bench scripts) ----
    CpuFeatures cpu;
    cpu.detect();
    const bool use_avx2 = cpu.avx2;

    std::fprintf(stderr,
                 "[v4] CPUID: SSE4.2=%d AVX=%d AVX2=%d FMA=%d BMI2=%d AES=%d SHA=%d  -> %s\n",
                 cpu.sse42, cpu.avx, cpu.avx2, cpu.fma, cpu.bmi2, cpu.aes, cpu.sha,
                 use_avx2 ? "AVX2 path" : "scalar path");

    const U128 A = build_A();
    const U128 u0(1, 0);
    const uint64_t mu = N / (uint64_t)T;

    // Pre-build per-thread start states identically to v3 (leapfrog).
    std::vector<U128> u_start(T);
    u_start[0] = u0;
    if (T > 1) {
        const U128 A_mu = pow_mod_2_128(A, mu);
        for (int t = 1; t < T; ++t) {
            u_start[t] = mul_mod_2_128(u_start[t - 1], A_mu);
        }
    }

    std::vector<double> buf(N);

    auto t0 = std::chrono::high_resolution_clock::now();

    double checksum = 0.0;
    #pragma omp parallel reduction(+:checksum) num_threads(T)
    {
        const int tid = omp_get_thread_num();
        const uint64_t start = (uint64_t)tid * mu;
        const uint64_t end   = (tid == T - 1) ? N : (start + mu);
        const uint64_t cnt   = end - start;

        double s = 0.0;
        if (use_avx2) {
            s = avx2_loop_one_thread(buf.data() + start, u_start[tid], A, cnt);
        } else {
            s = scalar_loop_one_thread(buf.data() + start, u_start[tid], A, cnt);
        }
        checksum += s;
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    const uint64_t to_dump = (N < 100ULL) ? N : 100ULL;
    FILE* f = std::fopen(out_path, "w");
    if (!f) { std::printf("cannot open %s\n", out_path); return 2; }
    std::fprintf(f, "# Parallel 128-bit MCG (v4: ISA-aware, %s)\n",
                 use_avx2 ? "AVX2 4-way ILP" : "скалярный запасной вариант");
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

    std::printf("PAR4 N=%llu T=%d time_ms=%.3f checksum=%.6f\n",
                (unsigned long long)N, T, ms, checksum);
    return 0;
}
