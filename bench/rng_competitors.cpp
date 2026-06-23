// rng_competitors.cpp -- reference RNG benchmark harness.
//
// Implements multiple random number generators in a single binary, with
// unified CLI and stdout format compatible with the existing bench
// scripts. Algorithms:
//
//   parmonc_lcg     : the same 128-bit MCG as our v3 (canonical PARMONC
//                     formula), used as a representative of v1..v5 for
//                     statistical-quality comparison. Numerically
//                     identical to v3 (checksum 499944.093122 at N=1e6).
//
//   mt19937_64      : std::mt19937_64 from <random>. The C++ standard
//                     library reference, well known in Monte Carlo.
//                     Period 2^19937, state ~2.5 KiB.
//
//   xoshiro256++    : Blackman & Vigna, public domain. Modern fast
//                     non-cryptographic RNG. Period 2^256, state 32 B.
//                     De facto default in Julia, Lua, Erlang.
//
//   splitmix64      : public domain. Trivially simple, very fast.
//                     Period 2^64 (intentionally short -- included as
//                     a sanity counter-example, since its period is
//                     much shorter than what Monte Carlo really wants).
//
//   pcg64           : PCG XSL RR 128/64.
//                     128-bit LCG state with output permutation.
//
//   sprng_lcg64     : 64-bit LCG with prime addend using the classical
//                     constants used in SPRNG-like LCG64 implementations:
//                     x(n+1) = a*x(n) + c mod 2^64,
//                     a=2862933555777941757, c=3037000493.
//
//   mkl_mcg59_like  : MCG(13^13, 2^59)-style multiplicative generator
//                     matching the oneMKL MCG59 family definition.
//                     Implemented here without oneMKL dependency for a
//                     local speed benchmark baseline.
//
//   mrg32k3a        : L'Ecuyer combined multiple recursive generator
//                     MRG32k3a (two order-3 recurrences).
//
//   lehmer127_mersenne : Lehmer-style multiplicative generator over
//                        modulus (2^127 - 1), implemented with a
//                        Mersenne reduction.
//
//   philox4x32_10      : Random123-style counter-based generator Philox,
//                        10 rounds, 128-bit counter as 4x32 words.
//
//   threefry2x64_20    : Random123-style counter-based generator Threefry,
//                        20 rounds, 128-bit counter as 2x64 words.
//
// CLI:
//   rng_competitors --algo=<name> --N=<N> [--threads=<T>]
//                   [--dump-binary=<path>] [--dump-text=<path>]
//
// stdout (one line, parsed by bench_final.ps1):
//   EXT algo=<name> N=<N> T=<T> time_ms=<float> checksum=<float>
//
// stderr: only on errors / unknown args.
//
// Build:
//   g++ -O3 -std=c++17 -fopenmp rng_competitors.cpp -o rng_competitors.exe
//
// Notes on threading:
//   * parmonc_lcg uses leapfrog with pow_mod_2_128 (same as v3).
//   * The other generators use independent per-thread seeds derived from a
//     fixed master seed via splitmix64. This is the conventional way to
//     parallelize these RNGs and matches what production Monte Carlo
//     code does. It does NOT produce a single global sequence -- but
//     for throughput measurement and per-stream quality testing this is
//     exactly the apples-to-apples we want.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>
#include <omp.h>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

// =====================================================================
// PARMONC 128-bit MCG (identical arithmetic to rng_par_v3.cpp)
// =====================================================================
struct U128 {
    uint64_t lo, hi;
    U128(uint64_t l = 0, uint64_t h = 0) : lo(l), hi(h) {}
};

static inline U128 mul_mod_2_128(const U128& a, const U128& b) {
#if defined(_MSC_VER)
    uint64_t p_hi = 0;
    const uint64_t p_lo = _umul128(a.lo, b.lo, &p_hi);
    const uint64_t cross = a.lo * b.hi + a.hi * b.lo;
    return U128(p_lo, p_hi + cross);
#else
    using u128 = unsigned __int128;
    const u128 p00 = (u128)a.lo * (u128)b.lo;
    const u128 cross = (u128)a.lo * (u128)b.hi + (u128)a.hi * (u128)b.lo;
    return U128((uint64_t)p00, (uint64_t)((p00 >> 64) + cross));
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

static U128 build_A() {
    static const int M[10]  = {1941, 1821, 3812, 1310, 68, 2906, 2335, 2609, 6859, 1999};
    static const int sh[10] = {0, 13, 26, 39, 52, 65, 78, 91, 104, 117};
    U128 a;
    for (int i = 0; i < 10; ++i) a = add_shifted_small(a, (uint64_t)M[i], sh[i]);
    return a;
}

static U128 pow_mod(U128 base, uint64_t exp) {
    U128 r(1, 0);
    while (exp != 0) {
        if (exp & 1ULL) r = mul_mod_2_128(r, base);
        base = mul_mod_2_128(base, base);
        exp >>= 1;
    }
    return r;
}

// =====================================================================
// xoshiro256++ -- Blackman & Vigna, public domain
// http://prng.di.unimi.it/xoshiro256plusplus.c
// =====================================================================
struct Xoshiro256pp {
    uint64_t s[4];
    static inline uint64_t rotl(uint64_t x, int k) {
        return (x << k) | (x >> (64 - k));
    }
    inline uint64_t operator()() {
        const uint64_t result = rotl(s[0] + s[3], 23) + s[0];
        const uint64_t t = s[1] << 17;
        s[2] ^= s[0]; s[3] ^= s[1]; s[1] ^= s[2]; s[0] ^= s[3];
        s[2] ^= t;
        s[3] = rotl(s[3], 45);
        return result;
    }
};

// =====================================================================
// SplitMix64 -- public domain, used both as a standalone RNG and as a
// seed-stream generator for the others.
// =====================================================================
inline uint64_t splitmix64_step(uint64_t& state) {
    uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

struct SplitMix64 {
    uint64_t state;
    inline uint64_t operator()() { return splitmix64_step(state); }
};

// =====================================================================
// PCG64 (XSL RR 128/64):
//   state = state * MULT + inc  (mod 2^128)
//   output permutation from previous state.
// =====================================================================
struct Pcg64 {
    U128 state{};
    U128 inc{};

    static inline U128 add128(U128 a, U128 b) {
        U128 r{};
        r.lo = a.lo + b.lo;
        const uint64_t carry = (r.lo < a.lo) ? 1ULL : 0ULL;
        r.hi = a.hi + b.hi + carry;
        return r;
    }

    inline uint64_t operator()() {
        static const U128 MULT = {0x4385DF649FCCF645ULL, 0x2360ED051FC65DA4ULL};
        const U128 old = state;
        state = add128(mul_mod_2_128(old, MULT), inc);
        const uint64_t xsl = old.hi ^ old.lo;
        const uint32_t rot = static_cast<uint32_t>(old.hi >> 58); // top 6 bits
        return (xsl >> rot) | (xsl << ((64U - rot) & 63U));
    }
};

// =====================================================================
// SPRNG-like 64-bit LCG with prime addend:
//   x(n+1) = a*x(n) + c (mod 2^64)
// =====================================================================
struct SprngLcg64 {
    uint64_t state;
    static constexpr uint64_t A = 2862933555777941757ULL;
    static constexpr uint64_t C = 3037000493ULL;
    inline uint64_t operator()() {
        state = state * A + C;
        return state;
    }
};

// =====================================================================
// oneMKL-like MCG59 core:
//   x(n+1) = (13^13 * x(n)) mod 2^59
// We expose 64-bit values by left-shifting the 59-bit state.
// =====================================================================
struct Mcg59Like {
    uint64_t state;
    static constexpr uint64_t A = 302875106592253ULL;       // 13^13
    static constexpr uint64_t MASK59 = (1ULL << 59) - 1ULL; // modulus 2^59
    inline uint64_t operator()() {
        state = (state * A) & MASK59;
        if (state == 0ULL) state = 1ULL; // avoid zero-lock
        return state << 5;
    }
};

// =====================================================================
// MRG32k3a (L'Ecuyer):
//   x_n = (1403580*x_{n-2} - 810728*x_{n-3}) mod m1, m1=2^32-209
//   y_n = (527612*y_{n-1} - 1370589*y_{n-3}) mod m2, m2=2^32-22853
//   z_n = (x_n - y_n) mod m1
// =====================================================================
struct Mrg32k3a {
    static constexpr uint64_t M1 = 4294967087ULL;
    static constexpr uint64_t M2 = 4294944443ULL;
    uint64_t x0, x1, x2; // x_{n-1}, x_{n-2}, x_{n-3}
    uint64_t y0, y1, y2; // y_{n-1}, y_{n-2}, y_{n-3}

    inline uint64_t operator()() {
        int64_t p1 = (int64_t)(1403580ULL * x1) - (int64_t)(810728ULL * x2);
        p1 %= (int64_t)M1;
        if (p1 < 0) p1 += (int64_t)M1;

        int64_t p2 = (int64_t)(527612ULL * y0) - (int64_t)(1370589ULL * y2);
        p2 %= (int64_t)M2;
        if (p2 < 0) p2 += (int64_t)M2;

        x2 = x1; x1 = x0; x0 = (uint64_t)p1;
        y2 = y1; y1 = y0; y0 = (uint64_t)p2;

        int64_t z = p1 - p2;
        if (z <= 0) z += (int64_t)M1;
        // Spread entropy across high and low halves.
        return ((uint64_t)z << 32) ^ (uint64_t)p2;
    }
};

// =====================================================================
// Lehmer-style mod (2^127 - 1) with 127-bit state:
//   s_{n+1} = (A * s_n) mod (2^127 - 1), s_n in [1, M-1]
// Here A is a fixed odd 64-bit multiplier (benchmark target).
// =====================================================================
struct Lehmer127Mersenne {
    uint64_t lo; // low 64 bits
    uint64_t hi; // high 63 bits (bit 63 must stay 0)

    static constexpr uint64_t HI_MASK63 = 0x7fffffffffffffffULL;
    static constexpr uint64_t A = 0xda942042e4dd58b5ULL;

    inline void normalize_after_add() {
        // Fold carry from bit 127 back into bit 0 (Mersenne reduction).
        if (hi >> 63) {
            uint64_t c = hi >> 63;
            hi &= HI_MASK63;
            uint64_t old_lo = lo;
            lo += c;
            if (lo < old_lo) ++hi;
        }
        // If value == M (all ones in 127 bits), map to 0.
        if (hi == HI_MASK63 && lo == 0xffffffffffffffffULL) {
            hi = 0;
            lo = 0;
        }
        // Keep non-zero state for multiplicative generator.
        if ((hi | lo) == 0ULL) lo = 1ULL;
    }

    inline uint64_t operator()() {
        using u128 = unsigned __int128;
        const u128 p0 = (u128)lo * (u128)A; // 128-bit
        const u128 p1 = (u128)hi * (u128)A; // <= 127-bit

        const uint64_t l0 = (uint64_t)p0;
        const uint64_t p0_hi = (uint64_t)(p0 >> 64);
        const uint64_t p1_lo = (uint64_t)p1;
        const uint64_t p1_hi = (uint64_t)(p1 >> 64);

        const u128 mid = (u128)p0_hi + (u128)p1_lo;
        const uint64_t l1 = (uint64_t)mid;
        const uint64_t mid_carry = (uint64_t)(mid >> 64);
        const uint64_t l2 = p1_hi + mid_carry;

        // Split into low 127 bits + high tail.
        lo = l0;
        hi = (l1 & HI_MASK63);
        const uint64_t tail = (l1 >> 63) | (l2 << 1);

        // Add tail into low part, then fold if needed.
        uint64_t old_lo = lo;
        lo += tail;
        if (lo < old_lo) ++hi;
        normalize_after_add();
        return (hi << 1) ^ (lo >> 1);
    }
};

// =====================================================================
// Philox4x32-10
// =====================================================================
struct Philox4x32_10 {
    uint32_t c0, c1, c2, c3;
    uint32_t k0, k1;
    uint64_t spare;
    bool have_spare;

    static inline uint32_t mulhi32(uint32_t a, uint32_t b) {
        return static_cast<uint32_t>((static_cast<uint64_t>(a) * static_cast<uint64_t>(b)) >> 32);
    }

    static inline void bump_counter(uint32_t& x0, uint32_t& x1, uint32_t& x2, uint32_t& x3) {
        if (++x0 == 0U) {
            if (++x1 == 0U) {
                if (++x2 == 0U) {
                    ++x3;
                }
            }
        }
    }

    inline uint64_t operator()() {
        if (have_spare) {
            have_spare = false;
            return spare;
        }

        static constexpr uint32_t M0 = 0xD2511F53U;
        static constexpr uint32_t M1 = 0xCD9E8D57U;
        static constexpr uint32_t W0 = 0x9E3779B9U;
        static constexpr uint32_t W1 = 0xBB67AE85U;

        uint32_t x0 = c0, x1 = c1, x2 = c2, x3 = c3;
        uint32_t kk0 = k0, kk1 = k1;

        for (int round = 0; round < 10; ++round) {
            const uint32_t hi0 = mulhi32(M0, x0);
            const uint32_t lo0 = M0 * x0;
            const uint32_t hi1 = mulhi32(M1, x2);
            const uint32_t lo1 = M1 * x2;

            const uint32_t y0 = hi1 ^ x1 ^ kk0;
            const uint32_t y1 = lo1;
            const uint32_t y2 = hi0 ^ x3 ^ kk1;
            const uint32_t y3 = lo0;

            x0 = y0; x1 = y1; x2 = y2; x3 = y3;
            kk0 += W0;
            kk1 += W1;
        }

        bump_counter(c0, c1, c2, c3);

        const uint64_t out0 = (static_cast<uint64_t>(x1) << 32) | static_cast<uint64_t>(x0);
        const uint64_t out1 = (static_cast<uint64_t>(x3) << 32) | static_cast<uint64_t>(x2);
        spare = out1;
        have_spare = true;
        return out0;
    }
};

// =====================================================================
// Threefry2x64-20
// =====================================================================
struct Threefry2x64_20 {
    uint64_t c0, c1;
    uint64_t k0, k1;
    uint64_t spare;
    bool have_spare;

    static inline uint64_t rotl64(uint64_t x, unsigned r) {
        return (x << r) | (x >> (64U - r));
    }

    static inline void round_mix(uint64_t& x0, uint64_t& x1, unsigned r) {
        x0 += x1;
        x1 = rotl64(x1, r);
        x1 ^= x0;
    }

    static inline void bump_counter(uint64_t& x0, uint64_t& x1) {
        if (++x0 == 0ULL) ++x1;
    }

    inline uint64_t operator()() {
        if (have_spare) {
            have_spare = false;
            return spare;
        }

        static constexpr uint64_t KS_PARITY = 0x1BD11BDAA9FC1A22ULL;
        static constexpr unsigned R[8] = {16, 42, 12, 31, 16, 32, 24, 21};

        const uint64_t ks0 = k0;
        const uint64_t ks1 = k1;
        const uint64_t ks2 = KS_PARITY ^ ks0 ^ ks1;
        const uint64_t ks[3] = {ks0, ks1, ks2};

        uint64_t x0 = c0 + ks0;
        uint64_t x1 = c1 + ks1;

        for (int block = 0; block < 5; ++block) {
            round_mix(x0, x1, R[(block * 4 + 0) & 7]);
            round_mix(x0, x1, R[(block * 4 + 1) & 7]);
            round_mix(x0, x1, R[(block * 4 + 2) & 7]);
            round_mix(x0, x1, R[(block * 4 + 3) & 7]);
            const int s = block + 1;
            x0 += ks[s % 3];
            x1 += ks[(s + 1) % 3] + static_cast<uint64_t>(s);
        }

        bump_counter(c0, c1);

        spare = x1;
        have_spare = true;
        return x0;
    }
};

// =====================================================================
// uint64 -> double in [0, 1): top 53 bits / 2^53
// (identical to what v1..v5 use, so checksums of parmonc_lcg here
//  match the canonical 499944.093122 at N=1e6)
// =====================================================================
inline double u64_to_double(uint64_t u) {
    return (double)(u >> 11) * (1.0 / 9007199254740992.0);
}

// =====================================================================
// Workers
// =====================================================================
static double work_parmonc(double* dst, U128 u, const U128& A, uint64_t cnt) {
    double sum = 0.0;
    for (uint64_t i = 0; i < cnt; ++i) {
        u = mul_mod_2_128(u, A);
        const double a = u64_to_double(u.hi);
        dst[i] = a;
        sum += a;
    }
    return sum;
}

template <typename Gen>
static double work_generic(double* dst, Gen& gen, uint64_t cnt) {
    double sum = 0.0;
    for (uint64_t i = 0; i < cnt; ++i) {
        const double a = u64_to_double(gen());
        dst[i] = a;
        sum += a;
    }
    return sum;
}

// =====================================================================
// Argument parsing
// =====================================================================
struct Args {
    std::string algo;
    uint64_t    N = 0;
    int         T = 0;
    std::string dump_binary;
    std::string dump_text;
};

static bool parse_kv(const char* arg, const char* prefix, std::string& out) {
    const size_t plen = std::strlen(prefix);
    if (std::strncmp(arg, prefix, plen) == 0) { out = arg + plen; return true; }
    return false;
}

static void usage(const char* prog) {
    std::printf("usage: %s --algo=<name> --N=<N> [--threads=<T>]\n", prog);
    std::printf("       [--dump-binary=<path>] [--dump-text=<path>]\n");
    std::printf("  algo: parmonc_lcg | mt19937_64 | xoshiro256pp | splitmix64 | pcg64 |\n");
    std::printf("        sprng_lcg64 | mkl_mcg59_like\n");
    std::printf("        mrg32k3a | lehmer127_mersenne | philox4x32_10 | threefry2x64_20\n");
}

// =====================================================================
// main
// =====================================================================
int main(int argc, char** argv) {
    Args args;
    args.T = omp_get_max_threads();

    for (int i = 1; i < argc; ++i) {
        std::string val;
        if      (parse_kv(argv[i], "--algo=",        val)) args.algo = val;
        else if (parse_kv(argv[i], "--N=",           val)) args.N = std::strtoull(val.c_str(), nullptr, 10);
        else if (parse_kv(argv[i], "--threads=",     val)) args.T = std::atoi(val.c_str());
        else if (parse_kv(argv[i], "--dump-binary=", val)) args.dump_binary = val;
        else if (parse_kv(argv[i], "--dump-text=",   val)) args.dump_text = val;
        else { std::fprintf(stderr, "[rng_competitors] unknown arg: %s\n", argv[i]); usage(argv[0]); return 1; }
    }

    if (args.algo.empty() || args.N == 0) { usage(argv[0]); return 1; }
    if (args.T < 1) args.T = 1;
    if (!(args.algo == "parmonc_lcg"   ||
          args.algo == "mt19937_64"    ||
          args.algo == "xoshiro256pp"  ||
          args.algo == "splitmix64"    ||
          args.algo == "pcg64"         ||
          args.algo == "sprng_lcg64"   ||
          args.algo == "mkl_mcg59_like"||
          args.algo == "mrg32k3a"      ||
          args.algo == "lehmer127_mersenne" ||
          args.algo == "philox4x32_10" ||
          args.algo == "threefry2x64_20")) {
        std::fprintf(stderr, "[rng_competitors] unknown algo: %s\n", args.algo.c_str());
        usage(argv[0]);
        return 1;
    }
    omp_set_num_threads(args.T);

    std::vector<double> buf(args.N);

    // --- parmonc_lcg precomputation: build A and per-thread starts ---
    U128 A;
    std::vector<U128> u_start;
    if (args.algo == "parmonc_lcg") {
        A = build_A();
        u_start.assign(args.T, U128(1, 0));
        const uint64_t mu = args.N / (uint64_t)args.T;
        if (args.T > 1) {
            const U128 A_mu = pow_mod(A, mu);
            for (int t = 1; t < args.T; ++t) {
                u_start[t] = mul_mod_2_128(u_start[t - 1], A_mu);
            }
        }
    }

    // ---- timed region ----
    auto t0 = std::chrono::high_resolution_clock::now();
    double checksum = 0.0;

    #pragma omp parallel reduction(+:checksum) num_threads(args.T)
    {
        const int      tid   = omp_get_thread_num();
        const uint64_t mu    = args.N / (uint64_t)args.T;
        const uint64_t start = (uint64_t)tid * mu;
        const uint64_t end   = (tid == args.T - 1) ? args.N : (start + mu);
        const uint64_t cnt   = end - start;

        // Per-thread seed derived from a fixed master via splitmix64.
        // Master = 0xCAFE0000 (arbitrary, deterministic across runs).
        uint64_t seed_state = 0xCAFE0000ULL ^ ((uint64_t)tid * 0x9E3779B97F4A7C15ULL);

        double s = 0.0;
        if (args.algo == "parmonc_lcg") {
            s = work_parmonc(buf.data() + start, u_start[tid], A, cnt);
        }
        else if (args.algo == "xoshiro256pp") {
            Xoshiro256pp gen;
            gen.s[0] = splitmix64_step(seed_state);
            gen.s[1] = splitmix64_step(seed_state);
            gen.s[2] = splitmix64_step(seed_state);
            gen.s[3] = splitmix64_step(seed_state);
            s = work_generic(buf.data() + start, gen, cnt);
        }
        else if (args.algo == "splitmix64") {
            SplitMix64 gen;
            gen.state = splitmix64_step(seed_state);
            s = work_generic(buf.data() + start, gen, cnt);
        }
        else if (args.algo == "mt19937_64") {
            std::mt19937_64 gen(splitmix64_step(seed_state));
            s = work_generic(buf.data() + start, gen, cnt);
        }
        else if (args.algo == "pcg64") {
            Pcg64 gen;
            gen.state.lo = splitmix64_step(seed_state);
            gen.state.hi = splitmix64_step(seed_state);
            gen.inc.lo   = splitmix64_step(seed_state) | 1ULL; // increment must be odd
            gen.inc.hi   = splitmix64_step(seed_state);
            s = work_generic(buf.data() + start, gen, cnt);
        }
        else if (args.algo == "sprng_lcg64") {
            SprngLcg64 gen;
            gen.state = splitmix64_step(seed_state);
            s = work_generic(buf.data() + start, gen, cnt);
        }
        else if (args.algo == "mkl_mcg59_like") {
            Mcg59Like gen;
            gen.state = (splitmix64_step(seed_state) & Mcg59Like::MASK59);
            if (gen.state == 0ULL) gen.state = 1ULL;
            s = work_generic(buf.data() + start, gen, cnt);
        }
        else if (args.algo == "mrg32k3a") {
            Mrg32k3a gen;
            gen.x0 = (splitmix64_step(seed_state) % (Mrg32k3a::M1 - 1ULL)) + 1ULL;
            gen.x1 = (splitmix64_step(seed_state) % (Mrg32k3a::M1 - 1ULL)) + 1ULL;
            gen.x2 = (splitmix64_step(seed_state) % (Mrg32k3a::M1 - 1ULL)) + 1ULL;
            gen.y0 = (splitmix64_step(seed_state) % (Mrg32k3a::M2 - 1ULL)) + 1ULL;
            gen.y1 = (splitmix64_step(seed_state) % (Mrg32k3a::M2 - 1ULL)) + 1ULL;
            gen.y2 = (splitmix64_step(seed_state) % (Mrg32k3a::M2 - 1ULL)) + 1ULL;
            s = work_generic(buf.data() + start, gen, cnt);
        }
        else if (args.algo == "lehmer127_mersenne") {
            Lehmer127Mersenne gen;
            gen.lo = splitmix64_step(seed_state);
            gen.hi = splitmix64_step(seed_state) & Lehmer127Mersenne::HI_MASK63;
            if ((gen.hi | gen.lo) == 0ULL) gen.lo = 1ULL;
            s = work_generic(buf.data() + start, gen, cnt);
        }
        else if (args.algo == "philox4x32_10") {
            Philox4x32_10 gen;
            gen.c0 = static_cast<uint32_t>(start);
            gen.c1 = static_cast<uint32_t>(start >> 32);
            gen.c2 = static_cast<uint32_t>(tid);
            gen.c3 = static_cast<uint32_t>(tid >> 16);
            gen.k0 = static_cast<uint32_t>(splitmix64_step(seed_state));
            gen.k1 = static_cast<uint32_t>(splitmix64_step(seed_state));
            gen.spare = 0ULL;
            gen.have_spare = false;
            s = work_generic(buf.data() + start, gen, cnt);
        }
        else if (args.algo == "threefry2x64_20") {
            Threefry2x64_20 gen;
            gen.c0 = start;
            gen.c1 = static_cast<uint64_t>(tid);
            gen.k0 = splitmix64_step(seed_state);
            gen.k1 = splitmix64_step(seed_state);
            gen.spare = 0ULL;
            gen.have_spare = false;
            s = work_generic(buf.data() + start, gen, cnt);
        }
        else {
            // guarded above; keep as a safety fallback
        }
        checksum += s;
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // ---- optional dumps (NOT timed) ----
    if (!args.dump_binary.empty()) {
        FILE* f = std::fopen(args.dump_binary.c_str(), "wb");
        if (!f) { std::fprintf(stderr, "[rng_competitors] cannot open %s\n", args.dump_binary.c_str()); return 2; }
        std::fwrite(buf.data(), sizeof(double), (size_t)args.N, f);
        std::fclose(f);
    }
    if (!args.dump_text.empty()) {
        FILE* f = std::fopen(args.dump_text.c_str(), "w");
        if (!f) { std::fprintf(stderr, "[rng_competitors] cannot open %s\n", args.dump_text.c_str()); return 2; }
        std::fprintf(f, "# %s, T=%d, N=%llu\n", args.algo.c_str(), args.T, (unsigned long long)args.N);
        const uint64_t to_dump = (args.N < 100ULL) ? args.N : 100ULL;
        for (uint64_t i = 0; i < to_dump; ++i) {
            std::fprintf(f, "%6llu\t%.15f\n", (unsigned long long)i, buf[(size_t)i]);
        }
        std::fclose(f);
    }

    std::printf("EXT algo=%s N=%llu T=%d time_ms=%.3f checksum=%.6f\n",
                args.algo.c_str(), (unsigned long long)args.N, args.T, ms, checksum);
    return 0;
}
