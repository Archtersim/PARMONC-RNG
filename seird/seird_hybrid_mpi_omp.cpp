#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <vector>
#if defined(_MSC_VER)
#include <intrin.h>
#endif

#ifdef USE_MPI
#include <mpi.h>
#endif

#ifdef _OPENMP
#include <omp.h>
#endif

namespace {

static inline const char* rng_core_label() {
#ifdef RNG_V1_DIGIT_STEP
    return "v1_digit_step";
#elif defined(RNG_V2_MANUAL_MUL)
    return "v2_manual_mul";
#elif defined(RNG_V4_BUFFERED)
    return "v4_buffered4";
#else
    return "v3_scalar";
#endif
}

struct U128 {
    uint64_t lo;
    uint64_t hi;

    U128(uint64_t lo_ = 0, uint64_t hi_ = 0) : lo(lo_), hi(hi_) {}

    bool is_zero() const { return lo == 0 && hi == 0; }
    bool odd() const { return (lo & 1ULL) != 0; }

    void shr1() {
        lo = (lo >> 1) | (hi << 63);
        hi >>= 1;
    }
};

static inline U128 mul_mod_2_128(const U128& a, const U128& b) {
#ifdef RNG_V2_MANUAL_MUL
    auto mul64x64 = [](uint64_t x, uint64_t y) -> U128 {
        const uint64_t xl = static_cast<uint32_t>(x);
        const uint64_t xh = x >> 32;
        const uint64_t yl = static_cast<uint32_t>(y);
        const uint64_t yh = y >> 32;

        const uint64_t ll = xl * yl;
        const uint64_t lh = xl * yh;
        const uint64_t hl = xh * yl;
        const uint64_t hh = xh * yh;
        const uint64_t mid = (ll >> 32) + (lh & 0xFFFFFFFFULL) + (hl & 0xFFFFFFFFULL);

        U128 r;
        r.lo = (ll & 0xFFFFFFFFULL) | (mid << 32);
        r.hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
        return r;
    };

    const U128 ll = mul64x64(a.lo, b.lo);
    const uint64_t cross = a.lo * b.hi + a.hi * b.lo;
    return U128(ll.lo, ll.hi + cross);
#elif defined(_MSC_VER)
    uint64_t p1_hi = 0;
    const uint64_t p1_lo = _umul128(a.lo, b.lo, &p1_hi);
    const uint64_t cross1_lo = a.lo * b.hi;
    const uint64_t cross2_lo = a.hi * b.lo;
    return U128(p1_lo, p1_hi + cross1_lo + cross2_lo);
#else
    using u128 = unsigned __int128;
    const u128 a0 = static_cast<u128>(a.lo);
    const u128 a1 = static_cast<u128>(a.hi);
    const u128 b0 = static_cast<u128>(b.lo);
    const u128 b1 = static_cast<u128>(b.hi);
    const u128 p00 = a0 * b0;
    const u128 cross = (a0 * b1) + (a1 * b0);
    const uint64_t lo = static_cast<uint64_t>(p00);
    const uint64_t hi = static_cast<uint64_t>((p00 >> 64) + cross);
    return U128(lo, hi);
#endif
}

static inline U128 add_shifted_small(U128 cur, uint64_t v, int shift) {
    if (shift < 64) {
        const uint64_t add_lo = v << shift;
        cur.lo += add_lo;
        if (cur.lo < add_lo) {
            ++cur.hi;
        }
        if (shift != 0) {
            cur.hi += (v >> (64 - shift));
        }
    } else {
        cur.hi += (v << (shift - 64));
    }
    return cur;
}

struct Rng128 {
    U128 state;

    static constexpr int M[10] = {1941, 1821, 3812, 1310, 68, 2906, 2335, 2609, 6859, 1999};

    static U128 multiplier() {
        constexpr int shifts[10] = {0, 13, 26, 39, 52, 65, 78, 91, 104, 117};
        U128 a(0, 0);
        for (int i = 0; i < 10; ++i) {
            a = add_shifted_small(a, static_cast<uint64_t>(M[i]), shifts[i]);
        }
        return a;
    }

    static const U128 A;

    explicit Rng128(uint64_t seed = 1) : state(seed == 0 ? U128(1, 0) : U128(seed, 0)) {}

#ifdef RNG_V1_DIGIT_STEP
    static U128 from_digits(const int u[10]) {
        U128 s(0, 0);
        constexpr int shifts[10] = {0, 13, 26, 39, 52, 65, 78, 91, 104, 117};
        for (int i = 0; i < 10; ++i) {
            s = add_shifted_small(s, static_cast<uint64_t>(u[i]), shifts[i]);
        }
        return s;
    }

    static void to_digits(const U128& s, int u[10]) {
        auto get_bits = [](const U128& x, int sh, uint64_t mask) -> int {
            uint64_t v = 0;
            if (sh < 64) {
                v = (x.lo >> sh);
                if (sh != 0) v |= (x.hi << (64 - sh));
            } else {
                v = (x.hi >> (sh - 64));
            }
            return static_cast<int>(v & mask);
        };
        for (int i = 0; i < 9; ++i) {
            u[i] = get_bits(s, 13 * i, 0x1FFFULL);
        }
        u[9] = get_bits(s, 117, 0x07FFULL);
    }
#endif

    static double u01_from_hi(uint64_t hi) {
        const uint64_t mantissa53 = hi >> 11;
        double x = static_cast<double>(mantissa53) * (1.0 / 9007199254740992.0); // 2^-53
        if (x <= 0.0) {
            x = std::numeric_limits<double>::min();
        }
        if (x >= 1.0) {
            x = std::nextafter(1.0, 0.0);
        }
        return x;
    }

#ifdef RNG_V1_DIGIT_STEP
    static double u01_from_state_legacy_digits(const U128& s) {
        int u[10];
        to_digits(s, u);
        static const double x[10] = {
            0.00000000000000000000000000000000000000293873587705571880,
            0.00000000000000000000000000000000002407412430484044800000,
            0.00000000000000000000000000000019721522630525295000000000,
            0.00000000000000000000000000161558713389263220000000000000,
            0.00000000000000000000001323488980084844300000000000000000,
            0.00000000000000000010842021724855044000000000000000000000,
            0.00000000000000088817841970012523000000000000000000000000,
            0.00000000000727595761418342590000000000000000000000000000,
            0.00000005960464477539062500000000000000000000000000000000,
            0.00048828125000000000000000000000000000000000000000000000
        };
        double y = 0.0;
        for (int i = 0; i < 10; ++i) {
            y += static_cast<double>(u[i]) * x[i];
        }
        if (y <= 0.0) y = std::numeric_limits<double>::min();
        if (y >= 1.0) y = std::nextafter(1.0, 0.0);
        return y;
    }
#endif

    static double u01_from_state(const U128& s) {
#ifdef RNG_V1_DIGIT_STEP
        return u01_from_state_legacy_digits(s);
#else
        return u01_from_hi(s.hi);
#endif
    }

    static double step_u01(U128& s) {
        s = mul_mod_2_128(s, A);
        return u01_from_state(s);
    }

    static U128 pow_mod_2_128(U128 base, uint64_t exp) {
        U128 result(1, 0);
        while (exp != 0) {
            if ((exp & 1ULL) != 0ULL) {
                result = mul_mod_2_128(result, base);
            }
            base = mul_mod_2_128(base, base);
            exp >>= 1;
        }
        return result;
    }

    void jump(uint64_t delta) {
        state = mul_mod_2_128(state, pow_mod_2_128(A, delta));
    }

    double next_u01() {
#ifdef RNG_V1_DIGIT_STEP
        // Побитово совместимо с историческим шагом обновления rnd128_ по 13-битным разрядам.
        int u[10];
        to_digits(state, u);
        int n, c0, c1, c2, c3, c4, c5, c6, c7, c8, c9;

        c0 = M[0] * u[0];
        c1 = M[0] * u[1] + M[1] * u[0];
        c2 = M[0] * u[2] + M[1] * u[1] + M[2] * u[0];
        c3 = M[0] * u[3] + M[1] * u[2] + M[2] * u[1] + M[3] * u[0];
        c4 = M[0] * u[4] + M[1] * u[3] + M[2] * u[2] + M[3] * u[1] + M[4] * u[0];
        c5 = M[0] * u[5] + M[1] * u[4] + M[2] * u[3] + M[3] * u[2] + M[4] * u[1] + M[5] * u[0];
        c6 = M[0] * u[6] + M[1] * u[5] + M[2] * u[4] + M[3] * u[3] + M[4] * u[2] + M[5] * u[1] + M[6] * u[0];
        c7 = M[0] * u[7] + M[1] * u[6] + M[2] * u[5] + M[3] * u[4] + M[4] * u[3] + M[5] * u[2] + M[6] * u[1] + M[7] * u[0];
        c8 = M[0] * u[8] + M[1] * u[7] + M[2] * u[6] + M[3] * u[5] + M[4] * u[4] + M[5] * u[3] + M[6] * u[2] + M[7] * u[1] + M[8] * u[0];
        c9 = M[0] * u[9] + M[1] * u[8] + M[2] * u[7] + M[3] * u[6] + M[4] * u[5] + M[5] * u[4] + M[6] * u[3] + M[7] * u[2] + M[8] * u[1] + M[9] * u[0];

        u[0] = c0 - ((c0 >> 13) << 13);
        n = c1 + (c0 >> 13); u[1] = n - ((n >> 13) << 13);
        n = c2 + (n >> 13);  u[2] = n - ((n >> 13) << 13);
        n = c3 + (n >> 13);  u[3] = n - ((n >> 13) << 13);
        n = c4 + (n >> 13);  u[4] = n - ((n >> 13) << 13);
        n = c5 + (n >> 13);  u[5] = n - ((n >> 13) << 13);
        n = c6 + (n >> 13);  u[6] = n - ((n >> 13) << 13);
        n = c7 + (n >> 13);  u[7] = n - ((n >> 13) << 13);
        n = c8 + (n >> 13);  u[8] = n - ((n >> 13) << 13);
        n = c9 + (n >> 13);  u[9] = n - ((n >> 11) << 11);

        state = from_digits(u);

        return u01_from_state(state);
#else
        state = mul_mod_2_128(state, A);
        return u01_from_state(state);
#endif
    }
};

const U128 Rng128::A = Rng128::multiplier();

struct StreamForRealization {
    Rng128 rng;
    U128 stride_factor;
    uint64_t block_len;
    uint64_t left_in_block;
    uint64_t generated_count;
    int rng_slowdown;
#ifdef RNG_V4_BUFFERED
    double cache4[4];
    int cache_pos;
    int cache_size;
#endif

    StreamForRealization(uint64_t base_seed, uint64_t start_offset, uint64_t stride_offset, uint64_t block)
        : rng(base_seed),
          stride_factor(Rng128::pow_mod_2_128(Rng128::A, stride_offset)),
          block_len(block),
          left_in_block(block),
          generated_count(0),
          rng_slowdown(1)
#ifdef RNG_V4_BUFFERED
          ,
          cache4{0.0, 0.0, 0.0, 0.0},
          cache_pos(0),
          cache_size(0)
#endif
    {
        rng.jump(start_offset);
    }

    void set_rng_slowdown(int k) { rng_slowdown = (k > 0 ? k : 1); }

    static inline void burn_cycles_for_state(const U128& s, int slowdown) {
        // Синтетическая нагрузка, используемая только для зондирования насыщения.
        if (slowdown <= 1) return;
        volatile uint64_t sink = s.lo ^ (s.hi << 1);
        const int iters = (slowdown - 1) * 64;
        for (int i = 0; i < iters; ++i) {
            sink ^= (s.lo + 0x9e3779b97f4a7c15ULL * static_cast<uint64_t>(i));
            sink = (sink << 7) | (sink >> 57);
            sink += (s.hi ^ 0xd1b54a32d192ed03ULL);
        }
        (void)sink;
    }

    inline void rng_burn_cycles() const { burn_cycles_for_state(rng.state, rng_slowdown); }

    double next_u01() {
#ifdef RNG_V4_BUFFERED
        if (cache_pos < cache_size) {
            --left_in_block;
            ++generated_count;
            const double y = cache4[cache_pos++];
            rng_burn_cycles();
            return y;
        }

        if (left_in_block == 0) {
            rng.state = mul_mod_2_128(rng.state, stride_factor);
            left_in_block = block_len;
        }

        if (left_in_block >= 4) {
            // Буферизованная генерация в стиле v4: продвинуть одну цепочку на A^4 и выдать 4 значения.
            const U128 s1 = mul_mod_2_128(rng.state, Rng128::A);
            const U128 s2 = mul_mod_2_128(s1, Rng128::A);
            const U128 s3 = mul_mod_2_128(s2, Rng128::A);
            const U128 s4 = mul_mod_2_128(s3, Rng128::A);
            rng.state = s4;

            cache4[0] = Rng128::u01_from_hi(s1.hi);
            cache4[1] = Rng128::u01_from_hi(s2.hi);
            cache4[2] = Rng128::u01_from_hi(s3.hi);
            cache4[3] = Rng128::u01_from_hi(s4.hi);
            cache_pos = 0;
            cache_size = 4;

            --left_in_block;
            ++generated_count;
            const double y = cache4[cache_pos++];
            rng_burn_cycles();
            return y;
        }

        // Safe scalar tail near block boundary.
        --left_in_block;
        ++generated_count;
        const double y = rng.next_u01();
        rng_burn_cycles();
        return y;
#else
        if (left_in_block == 0) {
            rng.state = mul_mod_2_128(rng.state, stride_factor);
            left_in_block = block_len;
        }
        --left_in_block;
        ++generated_count;
        const double y = rng.next_u01();
        rng_burn_cycles();
        return y;
#endif
    }
};

struct ParallelPrefetchStream {
    uint64_t base_seed;
    uint64_t start_offset;
    uint64_t stride_offset;
    uint64_t block_len;
    uint64_t generated_count;
    int rng_slowdown;
    int rng_threads;
    uint64_t prefetch_len;
    std::vector<double> buffer;
    size_t buffer_pos;

    ParallelPrefetchStream(uint64_t seed,
                           uint64_t start,
                           uint64_t stride,
                           uint64_t block,
                           int threads,
                           uint64_t prefetch)
        : base_seed(seed == 0 ? 1ULL : seed),
          start_offset(start),
          stride_offset(stride),
          block_len(block),
          generated_count(0),
          rng_slowdown(1),
          rng_threads(std::max(1, threads)),
          prefetch_len(std::max<uint64_t>(1, prefetch)),
          buffer(),
          buffer_pos(0) {}

    void set_rng_slowdown(int k) { rng_slowdown = (k > 0 ? k : 1); }

    static U128 state_before_draw(uint64_t base_seed,
                                  uint64_t start_offset,
                                  uint64_t stride_offset,
                                  uint64_t block_len,
                                  uint64_t draw_index) {
        const uint64_t block_id = draw_index / block_len;
        const uint64_t intra = draw_index % block_len;
        const uint64_t exp_before =
            start_offset + block_id * (stride_offset + block_len) + intra;
        Rng128 local(base_seed);
        local.jump(exp_before);
        return local.state;
    }

    void refill() {
        const uint64_t chunk = prefetch_len;
        buffer.assign(static_cast<size_t>(chunk), 0.0);
        buffer_pos = 0;

#ifdef _OPENMP
#pragma omp parallel num_threads(rng_threads)
        {
            const int tid = omp_get_thread_num();
            const int nth = omp_get_num_threads();
            const uint64_t begin = (chunk * static_cast<uint64_t>(tid)) / static_cast<uint64_t>(nth);
            const uint64_t end = (chunk * static_cast<uint64_t>(tid + 1)) / static_cast<uint64_t>(nth);
            if (begin < end) {
                const uint64_t first_draw = generated_count + begin;
                U128 state = state_before_draw(base_seed, start_offset, stride_offset, block_len, first_draw);
                const U128 stride_factor = Rng128::pow_mod_2_128(Rng128::A, stride_offset);
                uint64_t left_in_block = block_len - (first_draw % block_len);

                for (uint64_t i = begin; i < end; ++i) {
                    if (left_in_block == 0) {
                        state = mul_mod_2_128(state, stride_factor);
                        left_in_block = block_len;
                    }
                    --left_in_block;
                    const double y = Rng128::step_u01(state);
                    StreamForRealization::burn_cycles_for_state(state, rng_slowdown);
                    buffer[static_cast<size_t>(i)] = y;
                }
            }
        }
#else
        U128 state = state_before_draw(base_seed, start_offset, stride_offset, block_len, generated_count);
        const U128 stride_factor = Rng128::pow_mod_2_128(Rng128::A, stride_offset);
        uint64_t left_in_block = block_len - (generated_count % block_len);
        for (uint64_t i = 0; i < chunk; ++i) {
            if (left_in_block == 0) {
                state = mul_mod_2_128(state, stride_factor);
                left_in_block = block_len;
            }
            --left_in_block;
            const double y = Rng128::step_u01(state);
            StreamForRealization::burn_cycles_for_state(state, rng_slowdown);
            buffer[static_cast<size_t>(i)] = y;
        }
#endif
    }

    double next_u01() {
        if (buffer_pos >= buffer.size()) {
            refill();
        }
        ++generated_count;
        return buffer[buffer_pos++];
    }
};

struct Params {
    int Nr = 100;
    int Tmod = 90;
    int threads = 0;
    int rng_threads = 1;
    uint64_t block_len = 1000000ULL;
    uint64_t rng_prefetch = 65536ULL;
    int rng_slowdown = 1;

    double coef = 1.0;
    double alfaE = 0.999;
    double alfaI = 0.999;
    double kappa = 0.042;
    double ro = 0.922;
    double beta = 0.999;
    double mu = 0.0188;
    double p = 0.51;
    double tinc = 3.0;
};

Params parse_args(int argc, char** argv) {
    Params p;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--Nr" && i + 1 < argc) p.Nr = std::stoi(argv[++i]);
        else if (a == "--Tmod" && i + 1 < argc) p.Tmod = std::stoi(argv[++i]);
        else if (a == "--threads" && i + 1 < argc) p.threads = std::stoi(argv[++i]);
        else if (a == "--rng-threads" && i + 1 < argc) p.rng_threads = std::stoi(argv[++i]);
        else if (a == "--block" && i + 1 < argc) p.block_len = static_cast<uint64_t>(std::stoull(argv[++i]));
        else if (a == "--rng-prefetch" && i + 1 < argc) p.rng_prefetch = static_cast<uint64_t>(std::stoull(argv[++i]));
        else if (a == "--rng-slowdown" && i + 1 < argc) p.rng_slowdown = std::stoi(argv[++i]);
        else if (a == "--coef" && i + 1 < argc) p.coef = std::stod(argv[++i]);
        else if (a == "--help") {
            std::cout << "Использование: seird_hybrid [--Nr N] [--Tmod D] [--threads T] [--rng-threads G] [--block L] [--rng-prefetch P] [--coef C] [--rng-slowdown K]\n";
            std::exit(0);
        }
    }
    if (p.Nr <= 0 || p.Tmod <= 0 || p.block_len == 0 || p.rng_prefetch == 0 ||
        p.rng_slowdown <= 0 || p.rng_threads <= 0) {
        throw std::runtime_error("Invalid parameters: Nr, Tmod, block, rng-prefetch, rng-threads and rng-slowdown must be > 0");
    }
    return p;
}

void fill_tail(int from_day, int Tmod,
               std::vector<double>& S,
               std::vector<double>& E_inc,
               std::vector<double>& E,
               std::vector<double>& I,
               std::vector<double>& I_day,
               std::vector<double>& R,
               std::vector<double>& D) {
    for (int i = from_day + 1; i <= Tmod; ++i) {
        S[i] = S[from_day];
        E_inc[i] = E_inc[from_day];
        E[i] = E[from_day];
        I[i] = I[from_day];
        I_day[i] = 0;
        R[i] = R[from_day];
        D[i] = D[from_day];
    }
}

template <class StreamT>
void run_one_realization(const Params& p,
                         int global_r,
                         StreamT& stream,
                         std::vector<double>& out_f,
                         uint64_t& rng_used,
                         uint64_t& while_iters_used,
                         uint64_t event_counts[7]) {
    const int Tmod = p.Tmod;
    const int dim_events = 6;

    const double N0 = 2798170.0 * p.coef;
    const double E0 = 99.0 * p.coef;
    const double R0 = 24.0 * p.coef;
    const double S0 = N0 - E0 - R0;
    const double E_inc0 = 0.0;
    const double I0 = 0.0;
    const double D0 = 0.0;

    const double round_tinc = std::ceil(p.tinc);
    const double p_E_inc_2_E = std::exp(-p.tinc * p.kappa);

    std::vector<double> bm(dim_events, 0.0);
    std::vector<double> P(dim_events, 0.0);
    std::vector<double> S(Tmod + 1, 0.0), E_inc(Tmod + 1, 0.0), E(Tmod + 1, 0.0), I(Tmod + 1, 0.0),
        I_day(Tmod + 1, 0.0), R(Tmod + 1, 0.0), D(Tmod + 1, 0.0);

    std::deque<double> exit_inc_times;

    int index_tm_last = 0;
    S[index_tm_last] = S0;
    E_inc[index_tm_last] = E_inc0;
    E[index_tm_last] = E0;
    I[index_tm_last] = I0;
    I_day[index_tm_last] = 0.0;
    R[index_tm_last] = R0;
    D[index_tm_last] = D0;

    double tm = 0.0;

    while (tm <= Tmod) {
        ++while_iters_used;
        if (E_inc[index_tm_last] == 0 && E[index_tm_last] == 0 && I[index_tm_last] == 0) {
            fill_tail(index_tm_last, Tmod, S, E_inc, E, I, I_day, R, D);
            break;
        }

        bm[0] = (p.alfaI * I[index_tm_last] + p.alfaE * (E[index_tm_last] + E_inc[index_tm_last])) * (S[index_tm_last] / N0);
        bm[1] = p.ro * E[index_tm_last];
        bm[2] = p.beta * I[index_tm_last];
        bm[3] = p.mu * I[index_tm_last];
        bm[4] = p.ro * E_inc[index_tm_last];
        bm[5] = p.kappa * E[index_tm_last];

        double Bm = 0.0;
        for (double v : bm) Bm += v;
        if (Bm <= 0.0) {
            fill_tail(index_tm_last, Tmod, S, E_inc, E, I, I_day, R, D);
            break;
        }

        const double alfa_tau = stream.next_u01();
        const double psi_m = -std::log(alfa_tau) / Bm;
        const double tm_event = tm + psi_m;
        const double tm_exit = exit_inc_times.empty() ? std::numeric_limits<double>::infinity() : exit_inc_times.front();

        int min_kind = 0;
        double tm_next_real = tm_event;
        if (tm_exit < tm_event) {
            min_kind = 1;
            tm_next_real = tm_exit;
        }

        tm = tm_next_real;

        if (tm > Tmod) {
            for (int i = index_tm_last + 1; i <= Tmod; ++i) {
                S[i] = S[index_tm_last];
                E_inc[i] = E_inc[index_tm_last];
                E[i] = E[index_tm_last];
                I[i] = I[index_tm_last];
                I_day[i] = 0.0;
                R[i] = R[index_tm_last];
                D[i] = D[index_tm_last];
            }
            break;
        }

        const int index_tm_next = static_cast<int>(std::ceil(tm));
        const int skipped = index_tm_next - index_tm_last - 1;
        for (int i = 1; i <= skipped; ++i) {
            S[index_tm_last + i] = S[index_tm_last];
            E_inc[index_tm_last + i] = E_inc[index_tm_last];
            E[index_tm_last + i] = E[index_tm_last];
            I[index_tm_last + i] = I[index_tm_last];
            I_day[index_tm_last + i] = 0.0;
            R[index_tm_last + i] = R[index_tm_last];
            D[index_tm_last + i] = D[index_tm_last];
        }

        if (min_kind == 0) {
            for (int i = 0; i < dim_events; ++i) {
                P[i] = bm[i] / Bm;
            }

            int index_event = 0;
            double alfa_evt = stream.next_u01();
            for (int i = 0; i < dim_events; ++i) {
                alfa_evt -= P[i];
                if (alfa_evt <= 0.0) {
                    index_event = i;
                    break;
                }
            }

            S[index_tm_next] = S[index_tm_last];
            E_inc[index_tm_next] = E_inc[index_tm_last];
            E[index_tm_next] = E[index_tm_last];
            I[index_tm_next] = I[index_tm_last];
            I_day[index_tm_next] = I_day[index_tm_last];
            R[index_tm_next] = R[index_tm_last];
            D[index_tm_next] = D[index_tm_last];

            switch (index_event) {
                case 0:
                    event_counts[0]++;
                    S[index_tm_next] -= 1;
                    E_inc[index_tm_next] += 1;
                    exit_inc_times.push_back(tm + round_tinc);
                    break;
                case 1:
                    event_counts[1]++;
                    E[index_tm_next] -= 1;
                    R[index_tm_next] += 1;
                    break;
                case 2:
                    event_counts[2]++;
                    I[index_tm_next] -= 1;
                    R[index_tm_next] += 1;
                    break;
                case 3:
                    event_counts[3]++;
                    I[index_tm_next] -= 1;
                    D[index_tm_next] += 1;
                    break;
                case 4: {
                    event_counts[4]++;
                    E_inc[index_tm_next] -= 1;
                    R[index_tm_next] += 1;
                    if (!exit_inc_times.empty()) {
                        const auto size = static_cast<int>(exit_inc_times.size());
                        int j = static_cast<int>(stream.next_u01() * static_cast<double>(size));
                        if (j >= size) j = size - 1;
                        auto it = exit_inc_times.begin();
                        std::advance(it, j);
                        exit_inc_times.erase(it);
                    }
                    break;
                }
                case 5:
                    event_counts[5]++;
                    E[index_tm_next] -= 1;
                    I[index_tm_next] += 1;
                    I_day[index_tm_next] += 1;
                    break;
                default:
                    break;
            }
        } else {
            event_counts[6]++;
            S[index_tm_next] = S[index_tm_last];
            E_inc[index_tm_next] = E_inc[index_tm_last] - 1;
            E[index_tm_next] = E[index_tm_last];
            I[index_tm_next] = I[index_tm_last];
            I_day[index_tm_next] = I_day[index_tm_last];
            R[index_tm_next] = R[index_tm_last];
            D[index_tm_next] = D[index_tm_last];

            if (!exit_inc_times.empty()) {
                exit_inc_times.pop_front();
            }

            const double alfa = stream.next_u01();
            if (alfa <= p_E_inc_2_E) {
                E[index_tm_next] += 1;
            } else {
                I[index_tm_next] += 1;
                I_day[index_tm_next] += 1;
            }
        }

        index_tm_last = index_tm_next;
    }

    out_f.assign(Tmod + 2, 0.0);
    for (int i = 1; i <= Tmod; ++i) {
        out_f[i] = (I_day[i] - I_day[i - 1]) / p.p;
    }
    out_f[Tmod + 1] = (E[Tmod] + E_inc[Tmod]) * p.kappa / p.p;
    rng_used = stream.generated_count;

    (void)global_r;
}

void write_output(const Params& p,
                  const std::vector<double>& sumf,
                  const std::vector<double>& sumkvf,
                  int Nr_total,
                  double elapsed_sec) {
    const int Tmod = p.Tmod;

    std::vector<double> Matogf(Tmod + 2, 0.0), Dispf(Tmod + 2, 0.0), stat_errorf(Tmod + 2, 0.0);

    for (int i = 1; i <= Tmod + 1; ++i) {
        Matogf[i] = sumf[i] / static_cast<double>(Nr_total);
        if (Nr_total > 1) {
            Dispf[i] = (static_cast<double>(Nr_total) / static_cast<double>(Nr_total - 1)) *
                       ((sumkvf[i] / static_cast<double>(Nr_total)) - Matogf[i] * Matogf[i]);
            if (Dispf[i] < 0.0) Dispf[i] = 0.0;
            stat_errorf[i] = std::sqrt(Dispf[i] / static_cast<double>(Nr_total));
        }
    }

    {
        std::ofstream mat("Matogf.txt");
        mat << "day identified lval uval\n";
        mat << std::fixed << std::setprecision(6);
        for (int i = 1; i <= Tmod + 1; ++i) {
            const double m = Matogf[i] / p.coef;
            const double d = std::sqrt(Dispf[i]) / p.coef;
            mat << i << ' ' << m << ' ' << (m - d) << ' ' << (m + d) << '\n';
        }
    }

    {
        std::ofstream err("stat_errorf.txt");
        err << "day error\n";
        err << std::fixed << std::setprecision(6);
        for (int i = 1; i <= Tmod + 1; ++i) {
            err << i << ' ' << (stat_errorf[i] / p.coef) << '\n';
        }
    }

    {
        std::ofstream timing("time_elapsed.txt");
        timing << "Time elapsed: " << std::fixed << std::setprecision(6) << elapsed_sec << " seconds\n";
    }
}

} // namespace

int main(int argc, char** argv) {
#ifdef USE_MPI
    MPI_Init(&argc, &argv);
#endif

    int rank = 0;
    int size = 1;
#ifdef USE_MPI
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);
#endif

    auto t0 = std::chrono::steady_clock::now();

    Params params;
    try {
        params = parse_args(argc, argv);
    } catch (const std::exception& ex) {
        if (rank == 0) {
            std::cerr << "Parameter error: " << ex.what() << "\n";
        }
#ifdef USE_MPI
        MPI_Abort(MPI_COMM_WORLD, 1);
#else
        return 1;
#endif
    }

    if (params.rng_threads > 1 && params.threads != 1) {
        throw std::runtime_error("The mode --rng-threads > 1 is supported only with --threads 1 so that the SEIRD model stays single-threaded.");
    }

#ifdef _OPENMP
    if (params.threads > 0) {
        omp_set_num_threads(params.threads);
    }
#endif

    const int Nr_total = params.Nr;
    const int rem = Nr_total % size;
    const int base = Nr_total / size;
    const int local_count = base + (rank < rem ? 1 : 0);
    const int first_global = rank * base + std::min(rank, rem);

    std::vector<double> local_sumf(params.Tmod + 2, 0.0);
    std::vector<double> local_sumkvf(params.Tmod + 2, 0.0);
    uint64_t local_rng_calls = 0;
    uint64_t local_while_iters = 0;
    uint64_t local_events[7] = {0, 0, 0, 0, 0, 0, 0};

    const uint64_t seed0 = 1;
    const uint64_t stride_offset = static_cast<uint64_t>(Nr_total) * params.block_len;

    if (params.rng_threads > 1) {
        std::vector<double> f;
        for (int lr = 0; lr < local_count; ++lr) {
            const int global_r = first_global + lr;
            const uint64_t start_offset = static_cast<uint64_t>(global_r) * params.block_len;

            ParallelPrefetchStream stream(
                seed0, start_offset, stride_offset, params.block_len, params.rng_threads, params.rng_prefetch);
            stream.set_rng_slowdown(params.rng_slowdown);
            uint64_t rng_used = 0;
            uint64_t while_used = 0;
            uint64_t events_used[7] = {0, 0, 0, 0, 0, 0, 0};
            run_one_realization(params, global_r, stream, f, rng_used, while_used, events_used);
            local_rng_calls += rng_used;
            local_while_iters += while_used;
            for (int ei = 0; ei < 7; ++ei) {
                local_events[ei] += events_used[ei];
            }
            for (int i = 1; i <= params.Tmod + 1; ++i) {
                local_sumf[i] += f[i];
                local_sumkvf[i] += f[i] * f[i];
            }
        }
    } else {
#pragma omp parallel
        {
            std::vector<double> thr_sumf(params.Tmod + 2, 0.0);
            std::vector<double> thr_sumkvf(params.Tmod + 2, 0.0);
            std::vector<double> f;
            uint64_t thr_rng_calls = 0;
            uint64_t thr_while_iters = 0;
            uint64_t thr_events[7] = {0, 0, 0, 0, 0, 0, 0};

#pragma omp for schedule(dynamic)
            for (int lr = 0; lr < local_count; ++lr) {
                const int global_r = first_global + lr;
                const uint64_t start_offset = static_cast<uint64_t>(global_r) * params.block_len;

                StreamForRealization stream(seed0, start_offset, stride_offset, params.block_len);
                stream.set_rng_slowdown(params.rng_slowdown);
                uint64_t rng_used = 0;
                uint64_t while_used = 0;
                uint64_t events_used[7] = {0, 0, 0, 0, 0, 0, 0};
                run_one_realization(params, global_r, stream, f, rng_used, while_used, events_used);
                thr_rng_calls += rng_used;
                thr_while_iters += while_used;
                for (int ei = 0; ei < 7; ++ei) {
                    thr_events[ei] += events_used[ei];
                }

                for (int i = 1; i <= params.Tmod + 1; ++i) {
                    thr_sumf[i] += f[i];
                    thr_sumkvf[i] += f[i] * f[i];
                }
            }

#pragma omp critical
            {
                for (int i = 1; i <= params.Tmod + 1; ++i) {
                    local_sumf[i] += thr_sumf[i];
                    local_sumkvf[i] += thr_sumkvf[i];
                }
                local_rng_calls += thr_rng_calls;
                local_while_iters += thr_while_iters;
                for (int ei = 0; ei < 7; ++ei) {
                    local_events[ei] += thr_events[ei];
                }
            }
        }
    }

    std::vector<double> global_sumf(params.Tmod + 2, 0.0);
    std::vector<double> global_sumkvf(params.Tmod + 2, 0.0);
    uint64_t global_rng_calls = 0;
    uint64_t global_while_iters = 0;
    uint64_t global_events[7] = {0, 0, 0, 0, 0, 0, 0};

#ifdef USE_MPI
    MPI_Reduce(local_sumf.data(), global_sumf.data(), params.Tmod + 2, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(local_sumkvf.data(), global_sumkvf.data(), params.Tmod + 2, MPI_DOUBLE, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(&local_rng_calls, &global_rng_calls, 1, MPI_UNSIGNED_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(&local_while_iters, &global_while_iters, 1, MPI_UNSIGNED_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
    MPI_Reduce(local_events, global_events, 7, MPI_UNSIGNED_LONG_LONG, MPI_SUM, 0, MPI_COMM_WORLD);
#else
    global_sumf = local_sumf;
    global_sumkvf = local_sumkvf;
    global_rng_calls = local_rng_calls;
    global_while_iters = local_while_iters;
    for (int ei = 0; ei < 7; ++ei) {
        global_events[ei] = local_events[ei];
    }
#endif

    auto t1 = std::chrono::steady_clock::now();
    const double elapsed_sec = std::chrono::duration<double>(t1 - t0).count();

    if (rank == 0) {
        write_output(params, global_sumf, global_sumkvf, Nr_total, elapsed_sec);

        std::cout << "Done. Nr=" << params.Nr
                  << ", Tmod=" << params.Tmod
                  << ", block=" << params.block_len
                  << ", rng_threads=" << params.rng_threads
                  << ", rng_prefetch=" << params.rng_prefetch
                  << ", rng_slowdown=" << params.rng_slowdown
                  << ", RNG_stride_offset=" << stride_offset
                  << ", RNG_calls_total=" << global_rng_calls
                  << ", while_iters_total=" << global_while_iters
                  << ", RNG_core=" << rng_core_label()
                  << "\n";

        std::ofstream rng_calls_file("rng_calls_total.txt");
        rng_calls_file << global_rng_calls << "\n";
        std::ofstream prof_file("par_profile_metrics.txt");
        prof_file << "rng_calls " << global_rng_calls << "\n";
        prof_file << "while_iters " << global_while_iters << "\n";
        prof_file << "rng_threads " << params.rng_threads << "\n";
        prof_file << "rng_prefetch " << params.rng_prefetch << "\n";
        prof_file << "rng_slowdown " << params.rng_slowdown << "\n";
        prof_file << "rng_core " << rng_core_label() << "\n";
        prof_file << "event0 " << global_events[0] << "\n";
        prof_file << "event1 " << global_events[1] << "\n";
        prof_file << "event2 " << global_events[2] << "\n";
        prof_file << "event3 " << global_events[3] << "\n";
        prof_file << "event4 " << global_events[4] << "\n";
        prof_file << "event5 " << global_events[5] << "\n";
        prof_file << "exit_event " << global_events[6] << "\n";
    }

#ifdef USE_MPI
    MPI_Finalize();
#endif

    return 0;
}
