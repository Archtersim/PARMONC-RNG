// rng_streamer.cpp -- streaming RNG generator for PractRand.
//
// Writes raw 64-bit unsigned integers to stdout, little-endian native order
// (PractRand stdin64 input format).
//
// CLI:
//   rng_streamer --algo=<name> [--bytes=<N>] [--seed=<seed>]
//
// Supported algorithms:
//   v1 | v2 | v3 | v4 | v5 | parmonc_lcg | mt19937_64 | xoshiro256pp
//   splitmix64 | pcg64 | sprng_lcg64 | mkl_mcg59_like | mrg32k3a
//   lehmer127_mersenne
//
// Notes:
//   * v1..v5 are aliases to the same PARMONC core stream in this harness.
//   * For parmonc/v1..v5 we emit u.hi (top 64 bits of 128-bit state).
//   * --bytes=0 or omitted means stream until the pipe is closed.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

#if defined(_MSC_VER)
#include <intrin.h>
#endif
#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
#endif

// =====================================================================
// PARMONC 128-bit MCG
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

// =====================================================================
// xoshiro256++
// =====================================================================
struct Xoshiro256pp {
    uint64_t s[4];
    static inline uint64_t rotl(uint64_t x, int k) { return (x << k) | (x >> (64 - k)); }
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
// SplitMix64
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
// PCG64 (PCG XSL RR 128/64)
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
        const U128 old = state;
        static const U128 MULT = U128{0x4385DF649FCCF645ULL, 0x2360ED051FC65DA4ULL};
        state = add128(mul_mod_2_128(old, MULT), inc);
        const uint64_t xsl = old.hi ^ old.lo;
        const uint32_t rot = static_cast<uint32_t>(old.hi >> 58);
        return (xsl >> rot) | (xsl << ((64U - rot) & 63U));
    }
};

// =====================================================================
// sprng_lcg64
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
// mkl_mcg59_like
// =====================================================================
struct Mcg59Like {
    uint64_t state;
    static constexpr uint64_t A = 302875106592253ULL;       // 13^13
    static constexpr uint64_t MASK59 = (1ULL << 59) - 1ULL;
    inline uint64_t operator()() {
        state = (state * A) & MASK59;
        if (state == 0ULL) state = 1ULL;
        return state << 5;
    }
};

// =====================================================================
// mrg32k3a
// =====================================================================
struct Mrg32k3a {
    static constexpr uint64_t M1 = 4294967087ULL;
    static constexpr uint64_t M2 = 4294944443ULL;
    uint64_t x0, x1, x2;
    uint64_t y0, y1, y2;

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
        return ((uint64_t)z << 32) ^ (uint64_t)p2;
    }
};

// =====================================================================
// lehmer127_mersenne
// =====================================================================
struct Lehmer127Mersenne {
    uint64_t lo;
    uint64_t hi; // low 63 bits used

    static constexpr uint64_t HI_MASK63 = 0x7fffffffffffffffULL;
    static constexpr uint64_t A = 0xda942042e4dd58b5ULL;

    inline void normalize_after_add() {
        if (hi >> 63) {
            uint64_t c = hi >> 63;
            hi &= HI_MASK63;
            uint64_t old_lo = lo;
            lo += c;
            if (lo < old_lo) ++hi;
        }
        if (hi == HI_MASK63 && lo == 0xffffffffffffffffULL) {
            hi = 0; lo = 0;
        }
        if ((hi | lo) == 0ULL) lo = 1ULL;
    }

    inline uint64_t operator()() {
        using u128 = unsigned __int128;
        const u128 p0 = (u128)lo * (u128)A;
        const u128 p1 = (u128)hi * (u128)A;

        const uint64_t l0 = (uint64_t)p0;
        const uint64_t p0_hi = (uint64_t)(p0 >> 64);
        const uint64_t p1_lo = (uint64_t)p1;
        const uint64_t p1_hi = (uint64_t)(p1 >> 64);

        const u128 mid = (u128)p0_hi + (u128)p1_lo;
        const uint64_t l1 = (uint64_t)mid;
        const uint64_t mid_carry = (uint64_t)(mid >> 64);
        const uint64_t l2 = p1_hi + mid_carry;

        lo = l0;
        hi = (l1 & HI_MASK63);
        const uint64_t tail = (l1 >> 63) | (l2 << 1);

        uint64_t old_lo = lo;
        lo += tail;
        if (lo < old_lo) ++hi;
        normalize_after_add();
        return (hi << 1) ^ (lo >> 1);
    }
};

// =====================================================================
// Stream loop
// =====================================================================
static const size_t BUF_QWORDS = 8192;

static void stream_parmonc(uint64_t bytes_target) {
    const U128 A = build_A();
    U128 u(1, 0);
    uint64_t buf[BUF_QWORDS];

    const bool infinite = (bytes_target == 0);
    uint64_t bytes_left = bytes_target;

    while (infinite || bytes_left > 0) {
        size_t n = BUF_QWORDS;
        if (!infinite && (bytes_left / 8) < (uint64_t)n) n = (size_t)(bytes_left / 8);
        if (n == 0) break;

        for (size_t i = 0; i < n; ++i) {
            u = mul_mod_2_128(u, A);
            buf[i] = u.hi;
        }
        const size_t w = std::fwrite(buf, sizeof(uint64_t), n, stdout);
        if (w != n) return;
        if (!infinite) bytes_left -= (uint64_t)w * 8ULL;
    }
}

template <typename Gen>
static void stream_generic(Gen& gen, uint64_t bytes_target) {
    uint64_t buf[BUF_QWORDS];
    const bool infinite = (bytes_target == 0);
    uint64_t bytes_left = bytes_target;

    while (infinite || bytes_left > 0) {
        size_t n = BUF_QWORDS;
        if (!infinite && (bytes_left / 8) < (uint64_t)n) n = (size_t)(bytes_left / 8);
        if (n == 0) break;

        for (size_t i = 0; i < n; ++i) buf[i] = gen();
        const size_t w = std::fwrite(buf, sizeof(uint64_t), n, stdout);
        if (w != n) return;
        if (!infinite) bytes_left -= (uint64_t)w * 8ULL;
    }
}

// =====================================================================
// CLI
// =====================================================================
static bool parse_kv(const char* arg, const char* prefix, std::string& out) {
    const size_t plen = std::strlen(prefix);
    if (std::strncmp(arg, prefix, plen) == 0) { out = arg + plen; return true; }
    return false;
}

static void usage(const char* prog) {
    std::fprintf(stderr,
        "usage: %s --algo=<name> [--bytes=<N>] [--seed=<seed>]\n"
        "  algo: v1|v2|v3|v4|v5|parmonc_lcg|mt19937_64|xoshiro256pp|splitmix64|pcg64|\n"
        "        sprng_lcg64|mkl_mcg59_like|mrg32k3a|lehmer127_mersenne\n"
        "  bytes: number of bytes to emit (0 or omitted = stream until pipe closes)\n"
        "  seed:  master seed (default 0xCAFE0000)\n", prog);
}

int main(int argc, char** argv) {
    std::string algo;
    uint64_t bytes_target = 0;
    uint64_t seed_master = 0xCAFE0000ULL;

    for (int i = 1; i < argc; ++i) {
        std::string val;
        if      (parse_kv(argv[i], "--algo=",  val)) algo = val;
        else if (parse_kv(argv[i], "--bytes=", val)) bytes_target = std::strtoull(val.c_str(), nullptr, 0);
        else if (parse_kv(argv[i], "--seed=",  val)) seed_master = std::strtoull(val.c_str(), nullptr, 0);
        else { std::fprintf(stderr, "unknown arg: %s\n", argv[i]); usage(argv[0]); return 1; }
    }
    if (algo.empty()) { usage(argv[0]); return 1; }

#if defined(_WIN32)
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    uint64_t seed_state = seed_master;

    if (algo == "parmonc_lcg" || algo == "v1" || algo == "v2" || algo == "v3" || algo == "v4" || algo == "v5") {
        (void)seed_master;
        stream_parmonc(bytes_target);
    }
    else if (algo == "xoshiro256pp") {
        Xoshiro256pp gen;
        gen.s[0] = splitmix64_step(seed_state);
        gen.s[1] = splitmix64_step(seed_state);
        gen.s[2] = splitmix64_step(seed_state);
        gen.s[3] = splitmix64_step(seed_state);
        stream_generic(gen, bytes_target);
    }
    else if (algo == "splitmix64") {
        SplitMix64 gen;
        gen.state = seed_master;
        stream_generic(gen, bytes_target);
    }
    else if (algo == "mt19937_64") {
        std::mt19937_64 gen(splitmix64_step(seed_state));
        stream_generic(gen, bytes_target);
    }
    else if (algo == "pcg64") {
        Pcg64 gen;
        const uint64_t st_lo = splitmix64_step(seed_state);
        const uint64_t st_hi = splitmix64_step(seed_state);
        const uint64_t inc_lo = splitmix64_step(seed_state) | 1ULL; // odd increment required
        const uint64_t inc_hi = splitmix64_step(seed_state);
        gen.state = U128(st_lo, st_hi);
        gen.inc = U128(inc_lo, inc_hi);
        stream_generic(gen, bytes_target);
    }
    else if (algo == "sprng_lcg64") {
        SprngLcg64 gen;
        gen.state = splitmix64_step(seed_state);
        stream_generic(gen, bytes_target);
    }
    else if (algo == "mkl_mcg59_like") {
        Mcg59Like gen;
        gen.state = splitmix64_step(seed_state) & Mcg59Like::MASK59;
        if (gen.state == 0ULL) gen.state = 1ULL;
        stream_generic(gen, bytes_target);
    }
    else if (algo == "mrg32k3a") {
        Mrg32k3a gen;
        gen.x0 = (splitmix64_step(seed_state) % (Mrg32k3a::M1 - 1ULL)) + 1ULL;
        gen.x1 = (splitmix64_step(seed_state) % (Mrg32k3a::M1 - 1ULL)) + 1ULL;
        gen.x2 = (splitmix64_step(seed_state) % (Mrg32k3a::M1 - 1ULL)) + 1ULL;
        gen.y0 = (splitmix64_step(seed_state) % (Mrg32k3a::M2 - 1ULL)) + 1ULL;
        gen.y1 = (splitmix64_step(seed_state) % (Mrg32k3a::M2 - 1ULL)) + 1ULL;
        gen.y2 = (splitmix64_step(seed_state) % (Mrg32k3a::M2 - 1ULL)) + 1ULL;
        stream_generic(gen, bytes_target);
    }
    else if (algo == "lehmer127_mersenne") {
        Lehmer127Mersenne gen;
        gen.lo = splitmix64_step(seed_state);
        gen.hi = splitmix64_step(seed_state) & Lehmer127Mersenne::HI_MASK63;
        if ((gen.hi | gen.lo) == 0ULL) gen.lo = 1ULL;
        stream_generic(gen, bytes_target);
    }
    else {
        std::fprintf(stderr, "unknown algo: %s\n", algo.c_str());
        usage(argv[0]);
        return 1;
    }

    std::fflush(stdout);
    return 0;
}
