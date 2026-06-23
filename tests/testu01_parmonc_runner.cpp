#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>

extern "C" {
#include <testu01/unif01.h>
#include <testu01/bbattery.h>
}

#include "rng128.h"

namespace {

U128 g_state = initial_u();
U128 g_A = build_A_from_m(RNG_M);

// Продвинуть состояние и вернуть 32 случайных бита для TestU01.
// Используются старшие 32 бита старшего слова (та же стратегия "старших битов", что и в u_to_double).
unsigned int parmonc_bits(void) {
    g_state = mul128(g_state, g_A);
    const uint32_t v = static_cast<uint32_t>(g_state.hi >> 32);
    return static_cast<unsigned int>(v);
}

void usage(const char* exe) {
    std::cerr
        << "Использование:\n"
        << "  " << exe << " small [seed_lo]\n"
        << "  " << exe << " crush [seed_lo]\n"
        << "  " << exe << " big [seed_lo]\n\n"
        << "Примеры:\n"
        << "  " << exe << " small 1\n"
        << "  " << exe << " crush 1\n"
        << "  " << exe << " big 1\n";
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    const std::string battery = argv[1];
    uint64_t seed_lo = 1ULL;
    if (argc >= 3) {
        seed_lo = static_cast<uint64_t>(std::strtoull(argv[2], nullptr, 10));
        if (seed_lo == 0ULL) seed_lo = 1ULL;
    }

    g_state = U128{seed_lo, 0ULL};
    g_A = build_A_from_m(RNG_M);

    char gen_name[] = "PARMONC_MCG128";
    unif01_Gen* gen = unif01_CreateExternGenBits(gen_name, parmonc_bits);
    if (gen == nullptr) {
        std::cerr << "Не удалось создать обёртку генератора для TestU01.\n";
        return 2;
    }

    if (battery == "small") {
        bbattery_SmallCrush(gen);
    } else if (battery == "crush") {
        bbattery_Crush(gen);
    } else if (battery == "big") {
        bbattery_BigCrush(gen);
    } else {
        std::cerr << "Неизвестная батарея: " << battery << "\n";
        usage(argv[0]);
        unif01_DeleteExternGenBits(gen);
        return 1;
    }

    unif01_DeleteExternGenBits(gen);
    return 0;
}
