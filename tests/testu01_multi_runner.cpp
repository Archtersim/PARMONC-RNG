#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

extern "C" {
#include <testu01/bbattery.h>
#include <testu01/unif01.h>
}

#include "rng128.h"

namespace {

enum class GenCore {
    PARMONC_128_MCG,
    MT19937_64,
    XOSHIRO256PP,
    SPLITMIX64,
    PCG64,
    SPRNG_LCG64,
    MKL_MCG59_LIKE,
    MRG32K3A,
    LEHMER127_MERSENNE
};

enum class ParmoncExec {
    Seq,
    ParallelChunked
};

struct Xoshiro256pp {
    uint64_t s[4]{};

    static inline uint64_t rotl(uint64_t x, int k) {
        return (x << k) | (x >> (64 - k));
    }

    uint64_t next_u64() {
        const uint64_t result = rotl(s[0] + s[3], 23) + s[0];
        const uint64_t t = s[1] << 17;
        s[2] ^= s[0];
        s[3] ^= s[1];
        s[1] ^= s[2];
        s[0] ^= s[3];
        s[2] ^= t;
        s[3] = rotl(s[3], 45);
        return result;
    }
};

static inline uint64_t splitmix64_step(uint64_t& st) {
    uint64_t z = (st += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

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

    uint64_t next_u64() {
        // PCG XSL RR 128/64.
        const U128 old = state;
        static const U128 MULT = {0x4385DF649FCCF645ULL, 0x2360ED051FC65DA4ULL};
        state = add128(mul128(old, MULT), inc);
        const uint64_t xsl = old.hi ^ old.lo;
        const uint32_t rot = static_cast<uint32_t>(old.hi >> 58); // top 6 bits of 128-bit state
        return (xsl >> rot) | (xsl << ((64U - rot) & 63U));
    }
};

struct SprngLcg64 {
    uint64_t state = 1ULL;
    static constexpr uint64_t A = 2862933555777941757ULL;
    static constexpr uint64_t C = 3037000493ULL;
    inline uint64_t next_u64() {
        state = state * A + C;
        return state;
    }
};

struct Mcg59Like {
    uint64_t state = 1ULL;
    static constexpr uint64_t A = 302875106592253ULL;       // 13^13
    static constexpr uint64_t MASK59 = (1ULL << 59) - 1ULL; // modulus 2^59
    inline uint64_t next_u64() {
        state = (state * A) & MASK59;
        if (state == 0ULL) state = 1ULL;
        return state << 5;
    }
};

struct Mrg32k3a {
    static constexpr uint64_t M1 = 4294967087ULL;
    static constexpr uint64_t M2 = 4294944443ULL;
    uint64_t x0 = 1ULL, x1 = 1ULL, x2 = 1ULL;
    uint64_t y0 = 1ULL, y1 = 1ULL, y2 = 1ULL;

    inline uint64_t next_u64() {
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

struct Lehmer127Mersenne {
    uint64_t lo = 1ULL;
    uint64_t hi = 0ULL; // high 63 bits
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
            hi = 0ULL;
            lo = 0ULL;
        }
        if ((hi | lo) == 0ULL) lo = 1ULL;
    }

    inline uint64_t next_u64() {
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

struct Config {
    std::string gen_name;
    GenCore core = GenCore::PARMONC_128_MCG;
    std::string battery = "small";
    uint64_t seed = 1ULL;
    int threads = 0;
    uint64_t chunk = 1ULL << 20; // 1,048,576 outputs per refill
    bool list_only = false;
};

GenCore g_core = GenCore::PARMONC_128_MCG;
ParmoncExec g_par_exec = ParmoncExec::Seq;
U128 g_par_state = initial_u();
U128 g_par_A = build_A_from_m(RNG_M);
U128 g_par_seed = initial_u();
uint64_t g_par_produced = 0ULL;
int g_par_threads = 1;
uint64_t g_par_chunk = 1ULL << 20;
std::vector<uint32_t> g_par_buf;
size_t g_par_buf_pos = 0;
std::mt19937_64 g_mt;
Xoshiro256pp g_xoshiro;
uint64_t g_splitmix_state = 1ULL;
Pcg64 g_pcg;
SprngLcg64 g_sprng_lcg64;
Mcg59Like g_mcg59_like;
Mrg32k3a g_mrg32k3a;
Lehmer127Mersenne g_lehmer127;

void refill_parmonc_buffer() {
    const uint64_t total = g_par_chunk;
    g_par_buf.assign(static_cast<size_t>(total), 0U);
    const uint64_t base_index = g_par_produced;

#ifdef _OPENMP
    const int T = (g_par_threads > 0) ? g_par_threads : 1;
#pragma omp parallel num_threads(T)
    {
        const int tid = omp_get_thread_num();
        const int nt = omp_get_num_threads();
        const uint64_t start = (total * static_cast<uint64_t>(tid)) / static_cast<uint64_t>(nt);
        const uint64_t end = (total * static_cast<uint64_t>(tid + 1)) / static_cast<uint64_t>(nt);
        const uint64_t cnt = end - start;

        if (cnt > 0) {
            // For output index j (0-based within whole stream), value is from u_{j+1}.
            // We set local state to u_{base_index + start}, then do one step before each output.
            U128 u = mul128(g_par_seed, pow128(g_par_A, base_index + start));
            for (uint64_t i = 0; i < cnt; ++i) {
                u = mul128(u, g_par_A);
                g_par_buf[static_cast<size_t>(start + i)] = static_cast<uint32_t>(u.hi >> 32);
            }
        }
    }
#else
    U128 u = mul128(g_par_seed, pow128(g_par_A, base_index));
    for (uint64_t i = 0; i < total; ++i) {
        u = mul128(u, g_par_A);
        g_par_buf[static_cast<size_t>(i)] = static_cast<uint32_t>(u.hi >> 32);
    }
#endif

    g_par_produced += total;
    g_par_buf_pos = 0;
}

unsigned int next_bits() {
    uint64_t w = 0ULL;
    switch (g_core) {
        case GenCore::PARMONC_128_MCG:
            if (g_par_exec == ParmoncExec::Seq) {
                g_par_state = mul128(g_par_state, g_par_A);
                w = g_par_state.hi;
                break;
            } else {
                if (g_par_buf_pos >= g_par_buf.size()) {
                    refill_parmonc_buffer();
                }
                return static_cast<unsigned int>(g_par_buf[g_par_buf_pos++]);
            }
        case GenCore::MT19937_64:
            w = g_mt();
            break;
        case GenCore::XOSHIRO256PP:
            w = g_xoshiro.next_u64();
            break;
        case GenCore::SPLITMIX64:
            w = splitmix64_step(g_splitmix_state);
            break;
        case GenCore::PCG64:
            w = g_pcg.next_u64();
            break;
        case GenCore::SPRNG_LCG64:
            w = g_sprng_lcg64.next_u64();
            break;
        case GenCore::MKL_MCG59_LIKE:
            w = g_mcg59_like.next_u64();
            break;
        case GenCore::MRG32K3A:
            w = g_mrg32k3a.next_u64();
            break;
        case GenCore::LEHMER127_MERSENNE:
            w = g_lehmer127.next_u64();
            break;
    }
    return static_cast<unsigned int>(w >> 32);
}

void print_list(const char* exe) {
    std::cout
        << "Использование:\n"
        << "  " << exe << " --gen=<name> --battery=<small|crush|big> [--seed=<u64>] [--threads=<N>] [--chunk=<N>]\n"
        << "  " << exe << " <small|crush|big> [seed] [--gen=<name>]\n\n"
        << "Генераторы:\n"
        << "  v1            : эталон PARMONC (целевой вариант из приложения А)\n"
        << "  v2            : параллельная подача PARMONC в TestU01 (та же последовательность, что и у v1)\n"
        << "  v3            : параллельная подача PARMONC в TestU01 (та же последовательность, что и у v1)\n"
        << "  v4            : параллельная подача PARMONC в TestU01 (та же последовательность, что и у v1)\n"
        << "  v5            : параллельная подача PARMONC в TestU01 (та же последовательность, что и у v1)\n"
        << "  parmonc_lcg   : канонический 128-битный MCG PARMONC (та же последовательность, что и у v1)\n"
        << "  mt19937_64    : std::mt19937_64\n"
        << "  xoshiro256pp  : xoshiro256++\n"
        << "  splitmix64    : SplitMix64\n"
        << "  pcg64         : PCG XSL RR 128/64\n"
        << "  sprng_lcg64   : 64-битный LCG (константы типа SPRNG)\n"
        << "  mkl_mcg59_like: MCG(13^13, 2^59)-like baseline\n"
        << "  mrg32k3a      : L'Ecuyer combined MRG32k3a\n"
        << "  lehmer127_mersenne : Lehmer mod (2^127-1)\n\n"
        << "Примечания:\n"
        << "  v1 and parmonc_lcg use последовательный stepping.\n"
        << "  v2/v3/v4/v5 use chunked parallel refill and keep identical output stream.\n"
        << "  --threads applies to v2/v3/v4/v5; default is max available OpenMP threads.\n";
}

bool parse_gen_name(const std::string& s, GenCore& out_core, std::string& canonical, ParmoncExec& par_exec) {
    if (s == "v1" || s == "parmonc_lcg") {
        out_core = GenCore::PARMONC_128_MCG;
        canonical = "parmonc_lcg";
        par_exec = ParmoncExec::Seq;
        return true;
    }
    if (s == "v2" || s == "v3" || s == "v4" || s == "v5") {
        out_core = GenCore::PARMONC_128_MCG;
        canonical = "parmonc_lcg";
        par_exec = ParmoncExec::ParallelChunked;
        return true;
    }
    if (s == "mt19937_64") {
        out_core = GenCore::MT19937_64;
        canonical = "mt19937_64";
        par_exec = ParmoncExec::Seq;
        return true;
    }
    if (s == "xoshiro256pp") {
        out_core = GenCore::XOSHIRO256PP;
        canonical = "xoshiro256pp";
        par_exec = ParmoncExec::Seq;
        return true;
    }
    if (s == "splitmix64") {
        out_core = GenCore::SPLITMIX64;
        canonical = "splitmix64";
        par_exec = ParmoncExec::Seq;
        return true;
    }
    if (s == "pcg64") {
        out_core = GenCore::PCG64;
        canonical = "pcg64";
        par_exec = ParmoncExec::Seq;
        return true;
    }
    if (s == "sprng_lcg64") {
        out_core = GenCore::SPRNG_LCG64;
        canonical = "sprng_lcg64";
        par_exec = ParmoncExec::Seq;
        return true;
    }
    if (s == "mkl_mcg59_like") {
        out_core = GenCore::MKL_MCG59_LIKE;
        canonical = "mkl_mcg59_like";
        par_exec = ParmoncExec::Seq;
        return true;
    }
    if (s == "mrg32k3a") {
        out_core = GenCore::MRG32K3A;
        canonical = "mrg32k3a";
        par_exec = ParmoncExec::Seq;
        return true;
    }
    if (s == "lehmer127_mersenne") {
        out_core = GenCore::LEHMER127_MERSENNE;
        canonical = "lehmer127_mersenne";
        par_exec = ParmoncExec::Seq;
        return true;
    }
    return false;
}

bool parse_cli(int argc, char** argv, Config& cfg) {
    cfg = Config{};
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--help" || a == "-h" || a == "--list") {
            cfg.list_only = true;
            return true;
        }
        if (a.rfind("--gen=", 0) == 0) {
            cfg.gen_name = a.substr(6);
            continue;
        }
        if (a.rfind("--battery=", 0) == 0) {
            cfg.battery = a.substr(10);
            continue;
        }
        if (a.rfind("--seed=", 0) == 0) {
            cfg.seed = std::strtoull(a.substr(7).c_str(), nullptr, 10);
            continue;
        }
        if (a.rfind("--threads=", 0) == 0) {
            cfg.threads = std::atoi(a.substr(10).c_str());
            continue;
        }
        if (a.rfind("--chunk=", 0) == 0) {
            cfg.chunk = std::strtoull(a.substr(8).c_str(), nullptr, 10);
            continue;
        }

        // Обратносовместимая позиционная форма:
        //   <battery> [seed]
        if (a == "small" || a == "crush" || a == "big") {
            cfg.battery = a;
            if (i + 1 < argc) {
                const std::string next = argv[i + 1];
                if (!next.empty() && std::isdigit(static_cast<unsigned char>(next[0]))) {
                    cfg.seed = std::strtoull(next.c_str(), nullptr, 10);
                    ++i;
                }
            }
            continue;
        }

        std::cerr << "Неизвестный аргумент: " << a << "\n";
        return false;
    }

    if (cfg.gen_name.empty()) {
        cfg.gen_name = "v1";
    }
    if (cfg.seed == 0ULL) {
        cfg.seed = 1ULL;
    }
    if (cfg.chunk == 0ULL) {
        cfg.chunk = 1ULL << 20;
    }
    return true;
}

void seed_core(GenCore core, uint64_t seed, ParmoncExec par_exec, int threads, uint64_t chunk) {
    g_core = core;
    g_par_exec = par_exec;

    uint64_t sm = seed;
    if (sm == 0ULL) sm = 1ULL;

    g_par_A = build_A_from_m(RNG_M);
    g_par_state = U128{seed, 0ULL};
    g_par_seed = U128{seed, 0ULL};
    g_par_produced = 0ULL;
    g_par_buf.clear();
    g_par_buf_pos = 0;
    g_par_chunk = chunk;
#ifdef _OPENMP
    g_par_threads = (threads > 0) ? threads : omp_get_max_threads();
    if (g_par_threads < 1) g_par_threads = 1;
#else
    (void)threads;
    g_par_threads = 1;
#endif

    g_mt.seed(seed);

    g_xoshiro.s[0] = splitmix64_step(sm);
    g_xoshiro.s[1] = splitmix64_step(sm);
    g_xoshiro.s[2] = splitmix64_step(sm);
    g_xoshiro.s[3] = splitmix64_step(sm);
    if ((g_xoshiro.s[0] | g_xoshiro.s[1] | g_xoshiro.s[2] | g_xoshiro.s[3]) == 0ULL) {
        g_xoshiro.s[0] = 1ULL;
    }

    g_splitmix_state = seed;
    if (g_splitmix_state == 0ULL) g_splitmix_state = 1ULL;

    // Инициализировать PCG64 детерминированно из потока splitmix.
    uint64_t st_lo = splitmix64_step(sm);
    uint64_t st_hi = splitmix64_step(sm);
    uint64_t inc_lo = splitmix64_step(sm) | 1ULL; // требуется нечётный инкремент
    uint64_t inc_hi = splitmix64_step(sm);
    g_pcg.state = U128{st_lo, st_hi};
    g_pcg.inc = U128{inc_lo, inc_hi};

    g_sprng_lcg64.state = splitmix64_step(sm);
    if (g_sprng_lcg64.state == 0ULL) g_sprng_lcg64.state = 1ULL;

    g_mcg59_like.state = splitmix64_step(sm) & Mcg59Like::MASK59;
    if (g_mcg59_like.state == 0ULL) g_mcg59_like.state = 1ULL;

    g_mrg32k3a.x0 = (splitmix64_step(sm) % (Mrg32k3a::M1 - 1ULL)) + 1ULL;
    g_mrg32k3a.x1 = (splitmix64_step(sm) % (Mrg32k3a::M1 - 1ULL)) + 1ULL;
    g_mrg32k3a.x2 = (splitmix64_step(sm) % (Mrg32k3a::M1 - 1ULL)) + 1ULL;
    g_mrg32k3a.y0 = (splitmix64_step(sm) % (Mrg32k3a::M2 - 1ULL)) + 1ULL;
    g_mrg32k3a.y1 = (splitmix64_step(sm) % (Mrg32k3a::M2 - 1ULL)) + 1ULL;
    g_mrg32k3a.y2 = (splitmix64_step(sm) % (Mrg32k3a::M2 - 1ULL)) + 1ULL;

    g_lehmer127.lo = splitmix64_step(sm);
    g_lehmer127.hi = splitmix64_step(sm) & Lehmer127Mersenne::HI_MASK63;
    if ((g_lehmer127.lo | g_lehmer127.hi) == 0ULL) g_lehmer127.lo = 1ULL;
}

} // namespace

int main(int argc, char** argv) {
    Config cfg;
    if (!parse_cli(argc, argv, cfg)) {
        print_list(argv[0]);
        return 1;
    }
    if (cfg.list_only) {
        print_list(argv[0]);
        return 0;
    }

    GenCore core;
    ParmoncExec par_exec = ParmoncExec::Seq;
    std::string canonical;
    if (!parse_gen_name(cfg.gen_name, core, canonical, par_exec)) {
        std::cerr << "Неизвестный генератор: " << cfg.gen_name << "\n";
        print_list(argv[0]);
        return 1;
    }
    if (cfg.battery != "small" && cfg.battery != "crush" && cfg.battery != "big") {
        std::cerr << "Неизвестная батарея: " << cfg.battery << "\n";
        print_list(argv[0]);
        return 1;
    }

    seed_core(core, cfg.seed, par_exec, cfg.threads, cfg.chunk);

    std::string report_name = "GEN_" + cfg.gen_name + "_core_" + canonical;
    unif01_Gen* gen = unif01_CreateExternGenBits(
        const_cast<char*>(report_name.c_str()), next_bits);
    if (!gen) {
        std::cerr << "Не удалось создать обёртку TestU01.\n";
        return 2;
    }

    std::cout << "[testu01_multi_runner] gen=" << cfg.gen_name
              << " core=" << canonical
              << " battery=" << cfg.battery
              << " seed=" << cfg.seed
              << " режим=" << ((par_exec == ParmoncExec::ParallelChunked) ? "параллельный-блочный" : "последовательный")
              << " threads=" << g_par_threads
              << " chunk=" << g_par_chunk << "\n";

    if (cfg.battery == "small") {
        bbattery_SmallCrush(gen);
    } else if (cfg.battery == "crush") {
        bbattery_Crush(gen);
    } else {
        bbattery_BigCrush(gen);
    }

    unif01_DeleteExternGenBits(gen);
    return 0;
}
