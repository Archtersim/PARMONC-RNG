// rng_par.cpp — параллельная (OpenMP) реализация того же ГПСЧ.
//
// Стратегия (bf-генератор по Марченко, PARMONC):
//   разбиваем [0, N) на T равных блоков длины mu = N / T (последний поток
//   забирает остаток), поток t стартует с состояния
//       u_{t*mu} = u_0 * A^(t*mu)   (mod 2^128),
//   и крутит обычный шаг u <- u*A внутри своего блока. После такого
//   прыжка числа в разных потоках — это именно те же числа исходной
//   последовательности, и при склейке по порядку блоков мы получим
//   результат, идентичный последовательной версии.
//
// Оптимизации:
//   * прыжки A^(t*mu) считаются один раз во ВНЕ-параллельной части
//     (стоит O(T * log mu) умножений 128x128 — копейки на фоне N);
//   * каждый поток пишет в свой непересекающийся участок выходного
//     буфера — никакой синхронизации в горячем цикле, никакого
//     false sharing на горячем пути;
//   * контрольная сумма аккумулируется через OpenMP reduction(+:);
//   * ввод-вывод (запись первых 100 чисел) делается уже после счёта,
//     чтобы не мешать измерению времени.
//
// Запуск:  rng_par <N> <output_file> [num_threads]
//
// На stdout: N, число потоков, время в мс, контрольная сумма.

#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <chrono>
#include <vector>
#include <omp.h>
#include "rng128.h"

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: %s <N> <output_file> [num_threads]\n", argv[0]);
        return 1;
    }
    uint64_t N = std::strtoull(argv[1], nullptr, 10);
    const char* out_path = argv[2];

    int T = (argc >= 4) ? std::atoi(argv[3]) : omp_get_max_threads();
    if (T < 1) T = 1;
    omp_set_num_threads(T);

    // Параметры генератора.
    U128 A   = build_A_from_m(RNG_M);
    U128 u0  = initial_u();

    // Длина блока для каждого потока. Последний поток забирает остаток.
    uint64_t mu = N / (uint64_t)T;

    // Стартовые состояния для всех потоков: u_start[t] = u_0 * A^(t*mu).
    std::vector<U128> u_start((size_t)T);
    {
        // A^mu — общий "длинный прыжок", с него потом идёт пошаговая накрутка.
        U128 A_mu = pow128(A, mu);
        U128 cur  = u0;
        for (int t = 0; t < T; ++t) {
            u_start[(size_t)t] = cur;
            cur = mul128(cur, A_mu);
        }
    }

    // Буфер на N значений: каждый поток пишет в свой кусок без пересечений.
    // Это и есть основное хранилище для дампа в файл и для контрольной суммы.
    std::vector<double> buf((size_t)N);

    auto t0 = std::chrono::high_resolution_clock::now();

    double checksum = 0.0;

    #pragma omp parallel reduction(+:checksum) num_threads(T)
    {
        int tid = omp_get_thread_num();
        // Границы блока для текущего потока.
        uint64_t start = (uint64_t)tid * mu;
        uint64_t end   = (tid == T - 1) ? N : (start + mu);

        U128 u = u_start[(size_t)tid];
        double local_sum = 0.0;
        for (uint64_t i = start; i < end; ++i) {
            double a = rng_next(u, A);
            buf[(size_t)i] = a;
            local_sum += a;
        }
        checksum += local_sum;
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Запись первых 100 чисел уже после замера времени.
    const uint64_t to_dump = (N < 100ULL) ? N : 100ULL;
    FILE* f = std::fopen(out_path, "w");
    if (!f) { std::printf("cannot open %s\n", out_path); return 2; }
    std::fprintf(f, "# Parallel 128-bit congruential RNG (PARMONC, bf-jump)\n");
    std::fprintf(f, "# threads=%d, mu=%llu (block per thread)\n",
                 T, (unsigned long long)mu);
    std::fprintf(f, "# First %llu of %llu generated\n",
                 (unsigned long long)to_dump, (unsigned long long)N);
    std::fprintf(f, "#---------------------------------------------------------\n");
    for (uint64_t i = 0; i < to_dump; ++i) {
        std::fprintf(f, "%6llu\t%.15f\n", (unsigned long long)i, buf[(size_t)i]);
    }
    std::fclose(f);

    std::printf("PAR N=%llu T=%d time_ms=%.3f checksum=%.6f\n",
                (unsigned long long)N, T, ms, checksum);
    return 0;
}
