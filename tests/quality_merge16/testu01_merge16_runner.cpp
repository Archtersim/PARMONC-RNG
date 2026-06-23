#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

extern "C" {
#include <testu01/bbattery.h>
#include <testu01/unif01.h>
}

#include "merge16_common.h"

namespace {

struct Config {
    std::string gen_name = "mt19937_64";
    std::string battery = "small";
    std::string merge_name = "roundrobin";
    uint64_t seed = 0xCAFE0000ULL;
    int threads = 16;
    uint64_t block_words = 4096;
};

Merge16Engine g_engine;

unsigned int next_bits_merge16() {
    return static_cast<unsigned int>(g_engine.next_u64() >> 32);
}

bool parse_cli(int argc, char** argv, Config& cfg) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a.rfind("--gen=", 0) == 0) {
            cfg.gen_name = a.substr(6);
        } else if (a.rfind("--battery=", 0) == 0) {
            cfg.battery = a.substr(10);
        } else if (a.rfind("--seed=", 0) == 0) {
            cfg.seed = std::strtoull(a.substr(7).c_str(), nullptr, 10);
        } else if (a.rfind("--threads=", 0) == 0) {
            cfg.threads = std::atoi(a.substr(10).c_str());
        } else if (a.rfind("--merge=", 0) == 0) {
            cfg.merge_name = a.substr(8);
        } else if (a.rfind("--block-words=", 0) == 0) {
            cfg.block_words = std::strtoull(a.substr(14).c_str(), nullptr, 10);
        } else {
            std::cerr << "Unknown arg: " << a << "\n";
            return false;
        }
    }
    return true;
}

void usage(const char* exe) {
    std::cout
        << "Использование:\n"
        << "  " << exe << " --gen=<mt19937_64|xoshiro256pp> --battery=<small|crush|big>\n"
        << "             [--threads=16] [--merge=roundrobin|block] [--block-words=4096] [--seed=<u64>]\n";
}

} // namespace

int main(int argc, char** argv) {
    Config cfg;
    if (!parse_cli(argc, argv, cfg)) {
        usage(argv[0]);
        return 1;
    }

    if (cfg.threads < 1) cfg.threads = 1;
    if (cfg.battery != "small" && cfg.battery != "crush" && cfg.battery != "big") {
        usage(argv[0]);
        return 1;
    }

    if (!parse_gen_kind(cfg.gen_name, g_engine.gen)) {
        usage(argv[0]);
        return 1;
    }
    if (!parse_merge_mode(cfg.merge_name, g_engine.merge)) {
        usage(argv[0]);
        return 1;
    }

    g_engine.threads = cfg.threads;
    g_engine.block_words = (cfg.block_words == 0ULL) ? 4096ULL : cfg.block_words;
    g_engine.master_seed = cfg.seed;
    g_engine.seed_all();

    std::string report_name = "GEN_merge16_" + cfg.gen_name + "_" + cfg.merge_name;
    unif01_Gen* gen = unif01_CreateExternGenBits(
        const_cast<char*>(report_name.c_str()), next_bits_merge16);
    if (!gen) {
        std::cerr << "Не удалось создать обёртку TestU01.\n";
        return 2;
    }

    std::cout << "[testu01_merge16_runner] gen=" << cfg.gen_name
              << " battery=" << cfg.battery
              << " seed=" << cfg.seed
              << " threads=" << g_engine.threads
              << " merge=" << cfg.merge_name
              << " block_words=" << g_engine.block_words << "\n";

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

