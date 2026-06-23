#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#include <io.h>
#include <fcntl.h>
#endif

#include "merge16_common.h"

namespace {

struct Config {
    std::string gen_name = "mt19937_64";
    std::string merge_name = "roundrobin";
    uint64_t bytes_target = 0ULL;
    uint64_t seed = 0xCAFE0000ULL;
    int threads = 16;
    uint64_t block_words = 4096ULL;
};

bool parse_cli(int argc, char** argv, Config& cfg) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("--gen=", 0) == 0) {
            cfg.gen_name = a.substr(6);
        } else if (a.rfind("--merge=", 0) == 0) {
            cfg.merge_name = a.substr(8);
        } else if (a.rfind("--bytes=", 0) == 0) {
            cfg.bytes_target = std::strtoull(a.substr(8).c_str(), nullptr, 0);
        } else if (a.rfind("--seed=", 0) == 0) {
            cfg.seed = std::strtoull(a.substr(7).c_str(), nullptr, 0);
        } else if (a.rfind("--threads=", 0) == 0) {
            cfg.threads = std::atoi(a.substr(10).c_str());
        } else if (a.rfind("--block-words=", 0) == 0) {
            cfg.block_words = std::strtoull(a.substr(14).c_str(), nullptr, 0);
        } else {
            std::fprintf(stderr, "unknown arg: %s\n", argv[i]);
            return false;
        }
    }
    return true;
}

void usage(const char* prog) {
    std::fprintf(stderr,
        "usage: %s --gen=<mt19937_64|xoshiro256pp> [--merge=roundrobin|block]\n"
        "          [--threads=16] [--block-words=4096] [--bytes=<N>] [--seed=<seed>]\n",
        prog);
}

} // namespace

int main(int argc, char** argv) {
    Config cfg;
    if (!parse_cli(argc, argv, cfg)) {
        usage(argv[0]);
        return 1;
    }
    if (cfg.threads < 1) cfg.threads = 1;

    Merge16Engine engine;
    if (!parse_gen_kind(cfg.gen_name, engine.gen)) {
        usage(argv[0]);
        return 1;
    }
    if (!parse_merge_mode(cfg.merge_name, engine.merge)) {
        usage(argv[0]);
        return 1;
    }
    engine.threads = cfg.threads;
    engine.block_words = (cfg.block_words == 0ULL) ? 4096ULL : cfg.block_words;
    engine.master_seed = cfg.seed;
    engine.seed_all();

#if defined(_WIN32)
    _setmode(_fileno(stdout), _O_BINARY);
#endif

    constexpr size_t BUF_QWORDS = 8192;
    uint64_t buf[BUF_QWORDS];

    const bool infinite = (cfg.bytes_target == 0ULL);
    uint64_t bytes_left = cfg.bytes_target;

    while (infinite || bytes_left > 0ULL) {
        size_t n = BUF_QWORDS;
        if (!infinite && (bytes_left / 8ULL) < static_cast<uint64_t>(n)) {
            n = static_cast<size_t>(bytes_left / 8ULL);
        }
        if (n == 0) break;

        for (size_t i = 0; i < n; ++i) {
            buf[i] = engine.next_u64();
        }
        const size_t w = std::fwrite(buf, sizeof(uint64_t), n, stdout);
        if (w != n) return 0;
        if (!infinite) bytes_left -= static_cast<uint64_t>(w) * 8ULL;
    }

    std::fflush(stdout);
    return 0;
}

