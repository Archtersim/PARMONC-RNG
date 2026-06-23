#ifndef RNG128_H
#define RNG128_H
//
// Извлечённый и переписанный 128-битный мультипликативный конгруэнтный
// генератор из poison_incub_aka_SEIRD_2.cpp (rnd128_).
//
// Исходный код хранил u в массиве int u[10] по основанию 2^13 (низшая
// "цифра" — 11 бит). Это эквивалентно одному большому числу
//     u = sum_{k=0..9} u[k] * 2^(13*k)     (старшая цифра — 11 бит, итого 128 бит)
// и шагу u <- u * A (mod 2^128), где
//     A = sum_{k=0..9} m[k] * 2^(13*k).
//
// Эта формулировка совпадает с PARMONC (М. А. Марченко): u_n = u_{n-1}*A mod 2^128,
// alpha_n = u_n * 2^-128. Поэтому распараллеливание делается стандартным
// "прыжком": u_{t*mu} = u_0 * A^(t*mu) mod 2^128 (bf-генератор).
//
// Состояние храним как пару uint64_t: u = hi*2^64 + lo. Это переносимо
// между MSVC / GCC / Clang, не требует __int128.

#include <cstdint>

struct U128 {
    uint64_t lo;
    uint64_t hi;
};

// 64x64 -> 128 без расширений компилятора.
static inline U128 mul64x64(uint64_t x, uint64_t y) {
    uint64_t xl = (uint32_t)x, xh = x >> 32;
    uint64_t yl = (uint32_t)y, yh = y >> 32;

    uint64_t ll = xl * yl;
    uint64_t lh = xl * yh;
    uint64_t hl = xh * yl;
    uint64_t hh = xh * yh;

    uint64_t mid = (ll >> 32) + (lh & 0xFFFFFFFFULL) + (hl & 0xFFFFFFFFULL);

    U128 r;
    r.lo = (ll & 0xFFFFFFFFULL) | (mid << 32);
    r.hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
    return r;
}

// (a * b) mod 2^128. Нужны только младшие 128 бит произведения.
static inline U128 mul128(U128 a, U128 b) {
    U128 ll = mul64x64(a.lo, b.lo);
    uint64_t cross = a.lo * b.hi + a.hi * b.lo; // обрезается до 64 бит — это ок
    U128 r;
    r.lo = ll.lo;
    r.hi = ll.hi + cross;
    return r;
}

// base^exp mod 2^128, бинарным возведением в степень.
static inline U128 pow128(U128 base, uint64_t exp) {
    U128 res = {1ULL, 0ULL};
    while (exp) {
        if (exp & 1ULL) res = mul128(res, base);
        base = mul128(base, base);
        exp >>= 1;
    }
    return res;
}

// Восстановление 128-битного множителя A из коэффициентов исходного rnd128_:
// A = sum_{k=0..9} m[k] * 2^(13*k).
static inline U128 build_A_from_m(const int m[10]) {
    U128 A = {0ULL, 0ULL};
    for (int k = 0; k < 10; ++k) {
        // Сдвиг 13*k бит влево числа m[k].
        int sh = 13 * k;
        uint64_t v = (uint64_t)m[k];
        U128 term;
        if (sh < 64) {
            term.lo = v << sh;
            term.hi = (sh == 0) ? 0ULL : (v >> (64 - sh));
        } else {
            term.lo = 0ULL;
            term.hi = v << (sh - 64);
        }
        // A += term (mod 2^128)
        uint64_t old_lo = A.lo;
        A.lo = A.lo + term.lo;
        uint64_t carry = (A.lo < old_lo) ? 1ULL : 0ULL;
        A.hi = A.hi + term.hi + carry;
    }
    return A;
}

// Начальное состояние u_0 в исходном rnd128_: { 1, 0, 0, ..., 0 }, т.е. u_0 = 1.
static inline U128 initial_u() {
    return U128{1ULL, 0ULL};
}

// alpha = u * 2^-128 in [0, 1). Берём верхние 53 бита для double.
static inline double u_to_double(U128 u) {
    uint64_t top53 = u.hi >> 11;
    return (double)top53 * (1.0 / 9007199254740992.0); // 1 / 2^53
}

// Один шаг: u <- u * A (mod 2^128), вернуть alpha.
static inline double rng_next(U128& u, const U128& A) {
    u = mul128(u, A);
    return u_to_double(u);
}

// Множитель m[10] из исходного генератора — выносим сюда, чтобы обе версии
// (последовательная и параллельная) видели одинаковые числа.
static const int RNG_M[10] = { 1941, 1821, 3812, 1310, 68, 2906, 2335, 2609, 6859, 1999 };

#endif
