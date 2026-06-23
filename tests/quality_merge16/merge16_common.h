#pragma once

#include <cstdint>
#include <random>
#include <string>
#include <vector>

enum class GenKind {
    MT19937_64,
    XOSHIRO256PP
};

enum class MergeMode {
    RoundRobin,
    Block
};

static inline uint64_t splitmix64_step_merge16(uint64_t& state) {
    uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

struct Xoshiro256ppMerge16 {
    uint64_t s[4]{};

    static inline uint64_t rotl(uint64_t x, int k) {
        return (x << k) | (x >> (64 - k));
    }

    inline uint64_t next_u64() {
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

struct Merge16Engine {
    GenKind gen = GenKind::MT19937_64;
    MergeMode merge = MergeMode::RoundRobin;
    int threads = 16;
    uint64_t block_words = 4096;
    uint64_t master_seed = 0xCAFE0000ULL;

    std::vector<std::mt19937_64> mts;
    std::vector<Xoshiro256ppMerge16> xos;

    int current_thread = 0;
    uint64_t current_block_pos = 0;

    void seed_all() {
        mts.clear();
        xos.clear();
        mts.resize(static_cast<size_t>(threads));
        xos.resize(static_cast<size_t>(threads));

        for (int tid = 0; tid < threads; ++tid) {
            uint64_t seed_state = master_seed ^ (static_cast<uint64_t>(tid) * 0x9E3779B97F4A7C15ULL);
            if (gen == GenKind::MT19937_64) {
                mts[static_cast<size_t>(tid)].seed(splitmix64_step_merge16(seed_state));
            } else {
                auto& g = xos[static_cast<size_t>(tid)];
                g.s[0] = splitmix64_step_merge16(seed_state);
                g.s[1] = splitmix64_step_merge16(seed_state);
                g.s[2] = splitmix64_step_merge16(seed_state);
                g.s[3] = splitmix64_step_merge16(seed_state);
                if ((g.s[0] | g.s[1] | g.s[2] | g.s[3]) == 0ULL) {
                    g.s[0] = 1ULL;
                }
            }
        }

        current_thread = 0;
        current_block_pos = 0;
    }

    inline uint64_t next_from_thread(int tid) {
        if (gen == GenKind::MT19937_64) {
            return mts[static_cast<size_t>(tid)]();
        }
        return xos[static_cast<size_t>(tid)].next_u64();
    }

    inline uint64_t next_u64() {
        uint64_t out = next_from_thread(current_thread);

        if (merge == MergeMode::RoundRobin) {
            current_thread++;
            if (current_thread >= threads) current_thread = 0;
        } else {
            current_block_pos++;
            if (current_block_pos >= block_words) {
                current_block_pos = 0;
                current_thread++;
                if (current_thread >= threads) current_thread = 0;
            }
        }
        return out;
    }
};

static inline bool parse_gen_kind(const std::string& s, GenKind& out) {
    if (s == "mt19937_64") {
        out = GenKind::MT19937_64;
        return true;
    }
    if (s == "xoshiro256pp") {
        out = GenKind::XOSHIRO256PP;
        return true;
    }
    return false;
}

static inline bool parse_merge_mode(const std::string& s, MergeMode& out) {
    if (s == "roundrobin") {
        out = MergeMode::RoundRobin;
        return true;
    }
    if (s == "block") {
        out = MergeMode::Block;
        return true;
    }
    return false;
}

