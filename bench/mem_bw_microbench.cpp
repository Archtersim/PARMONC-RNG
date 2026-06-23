#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _OPENMP
#include <omp.h>
#endif

namespace {

struct Args {
    std::string mode = "memset";
    std::size_t size_mb = 1024;
    int iters = 8;
    int threads = 1;
};

Args parse_args(int argc, char** argv) {
    Args a;
    for (int i = 1; i < argc; ++i) {
        std::string s(argv[i]);
        auto eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string k = s.substr(0, eq);
        std::string v = s.substr(eq + 1);
        if (k == "--mode") a.mode = v;
        else if (k == "--size-mb") a.size_mb = static_cast<std::size_t>(std::stoull(v));
        else if (k == "--iters") a.iters = std::max(1, std::stoi(v));
        else if (k == "--threads") a.threads = std::max(1, std::stoi(v));
    }
    return a;
}

double now_s() {
    using clock = std::chrono::steady_clock;
    auto t = clock::now().time_since_epoch();
    return std::chrono::duration<double>(t).count();
}

} // namespace

int main(int argc, char** argv) {
    Args a = parse_args(argc, argv);

    const std::size_t bytes = a.size_mb * 1024ULL * 1024ULL;
    const std::size_t n64 = bytes / sizeof(std::uint64_t);
    if (n64 == 0) {
        std::cerr << "size too small\n";
        return 2;
    }

    std::vector<std::uint64_t> src(n64, 0x1122334455667788ULL);
    std::vector<std::uint64_t> dst(n64, 0ULL);

#ifdef _OPENMP
    omp_set_dynamic(0);
    omp_set_num_threads(a.threads);
#endif

    auto do_memset = [&]() {
        std::memset(dst.data(), 0xA5, bytes);
    };
    auto do_memcpy = [&]() {
        std::memcpy(dst.data(), src.data(), bytes);
    };
    auto do_omp_store = [&]() {
#ifdef _OPENMP
        #pragma omp parallel for schedule(static)
        for (std::int64_t i = 0; i < static_cast<std::int64_t>(n64); ++i) {
            dst[static_cast<std::size_t>(i)] = static_cast<std::uint64_t>(i) * 0x9E3779B97F4A7C15ULL;
        }
#else
        for (std::size_t i = 0; i < n64; ++i) {
            dst[i] = static_cast<std::uint64_t>(i) * 0x9E3779B97F4A7C15ULL;
        }
#endif
    };
    auto do_omp_copy = [&]() {
#ifdef _OPENMP
        #pragma omp parallel for schedule(static)
        for (std::int64_t i = 0; i < static_cast<std::int64_t>(n64); ++i) {
            dst[static_cast<std::size_t>(i)] = src[static_cast<std::size_t>(i)];
        }
#else
        for (std::size_t i = 0; i < n64; ++i) {
            dst[i] = src[i];
        }
#endif
    };

    // Warmup
    if (a.mode == "memset") do_memset();
    else if (a.mode == "memcpy") do_memcpy();
    else if (a.mode == "omp_store") do_omp_store();
    else if (a.mode == "omp_copy") do_omp_copy();
    else {
        std::cerr << "unknown mode: " << a.mode << "\n";
        return 3;
    }

    const double t0 = now_s();
    for (int it = 0; it < a.iters; ++it) {
        if (a.mode == "memset") do_memset();
        else if (a.mode == "memcpy") do_memcpy();
        else if (a.mode == "omp_store") do_omp_store();
        else do_omp_copy();
    }
    const double t1 = now_s();
    const double dt = std::max(1e-12, t1 - t0);

    const double moved_write_bytes = static_cast<double>(bytes) * static_cast<double>(a.iters);
    const double moved_total_bytes =
        (a.mode == "memcpy" || a.mode == "omp_copy")
            ? moved_write_bytes * 2.0
            : moved_write_bytes;

    const double write_gbps = moved_write_bytes / dt / 1.0e9;
    const double total_gbps = moved_total_bytes / dt / 1.0e9;

    std::cout << std::fixed << std::setprecision(6)
              << "MEMBW mode=" << a.mode
              << " size_mb=" << a.size_mb
              << " iters=" << a.iters
              << " threads=" << a.threads
              << " sec=" << dt
              << " write_gbps=" << write_gbps
              << " total_gbps=" << total_gbps
              << "\n";

    return 0;
}

