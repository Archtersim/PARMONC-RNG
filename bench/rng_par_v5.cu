// rng_par_v5.cu -- CUDA implementation of the PARMONC 128-bit MCG.
//
// Same numerical sequence u_n = u_0 * A^n (mod 2^128) as v1..v4, but
// every random number is computed on the GPU.
//
// MEMORY LAYOUT (the part that changed in this revision)
//
// Threads inside one warp must write to ADJACENT memory positions on
// every iteration -- otherwise each warp issues 32 separate transactions
// to HBM instead of one coalesced 256-byte transaction, and effective
// bandwidth collapses to ~8% of the GPU's peak. The first version of
// this file used a per-thread layout (thread t writes positions
// [t*mu, t*mu+mu)), which is fine for cache-resident sizes but hits
// the bandwidth wall at N>=1e8.
//
// New layout: warp-local. A warp of 32 lanes claims a contiguous block
// of `iters_per_warp * 32` positions in the output buffer. On iteration
// i, lane `l` writes position `warp_offset + i*32 + l`. Adjacent lanes
// write adjacent doubles -- a single 256-byte coalesced store on
// Ampere/Ada/Hopper.
//
// Each lane keeps its own ГПСЧ state. Per-iteration the state advances
// by A^32 (precomputed on host), so what was "consecutive A multiplies
// per element" becomes "one A^32 multiply per warp-step". The number of
// state updates per element stays exactly 1, just spread differently.
//
// Per-thread initial jump: u = u_init * A^(start_pos + 1), where
// start_pos = warp_offset + lane_id. This places the first written
// value at the correct position in the global lattice -- the same
// sequence u_n = u_0 * A^n that all other versions produce.
//
// Verified against the scalar reference on CPU (rng_par_v3.cpp) on
// nine corner cases including non-multiples of 32 -- bit-identical.
//
// Build (CUDA Toolkit 12+):
//   nvcc -O3 -std=c++17 --use_fast_math -arch=sm_86 \
//        rng_par_v5.cu -o rng_par_v5.exe
//
// Запуск:  rng_par_v5 <N> <output_file> [block_size]
// В stdout: PAR5 N=<N> T=<gridDim*blockDim> time_ms=<ms> checksum=<sum>
// В stderr: сведения о GPU и выбранной геометрии запуска

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <cuda_runtime.h>

// ---------------------------------------------------------------------
#define CUDA_OK(call)                                                       \
    do {                                                                    \
        cudaError_t _err = (call);                                          \
        if (_err != cudaSuccess) {                                          \
            std::fprintf(stderr, "CUDA error %s at %s:%d: %s\n",            \
                         #call, __FILE__, __LINE__, cudaGetErrorString(_err)); \
            std::exit(3);                                                   \
        }                                                                   \
    } while (0)

// ---------------------------------------------------------------------
struct U128 {
    uint64_t lo;
    uint64_t hi;
    __host__ __device__ U128() : lo(0), hi(0) {}
    __host__ __device__ U128(uint64_t l, uint64_t h) : lo(l), hi(h) {}
};

__device__ static inline U128 mul_mod_2_128_dev(const U128& a, const U128& b) {
    const uint64_t p_lo  = a.lo * b.lo;
    const uint64_t p_hi  = __umul64hi(a.lo, b.lo);
    const uint64_t cross = a.lo * b.hi + a.hi * b.lo;
    return U128(p_lo, p_hi + cross);
}

// Host multiply: portable, no compiler-specific intrinsics. Compiles
// equivalently to one MUL on x86_64 under both gcc and MSVC.
__host__ static inline uint64_t mulhi64_portable(uint64_t a, uint64_t b) {
    const uint64_t a_lo = (uint32_t)a;
    const uint64_t a_hi = a >> 32;
    const uint64_t b_lo = (uint32_t)b;
    const uint64_t b_hi = b >> 32;
    const uint64_t ll = a_lo * b_lo;
    const uint64_t lh = a_lo * b_hi;
    const uint64_t hl = a_hi * b_lo;
    const uint64_t hh = a_hi * b_hi;
    const uint64_t mid = (ll >> 32) + (uint32_t)lh + (uint32_t)hl;
    return hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
}

__host__ static inline U128 mul_mod_2_128_host(const U128& a, const U128& b) {
    const uint64_t p_lo  = a.lo * b.lo;
    const uint64_t p_hi  = mulhi64_portable(a.lo, b.lo);
    const uint64_t cross = a.lo * b.hi + a.hi * b.lo;
    return U128(p_lo, p_hi + cross);
}

// ---------------------------------------------------------------------
static const int RNG_M_HOST[10] = {1941, 1821, 3812, 1310, 68, 2906, 2335, 2609, 6859, 1999};

__host__ static inline U128 add_shifted_small_host(U128 cur, uint64_t v, int shift) {
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

__host__ static U128 build_A_host() {
    static const int shifts[10] = {0, 13, 26, 39, 52, 65, 78, 91, 104, 117};
    U128 a;
    for (int i = 0; i < 10; ++i) {
        a = add_shifted_small_host(a, (uint64_t)RNG_M_HOST[i], shifts[i]);
    }
    return a;
}

__host__ static U128 pow_mod_2_128_host(U128 base, uint64_t exp) {
    U128 r(1, 0);
    while (exp != 0) {
        if (exp & 1ULL) r = mul_mod_2_128_host(r, base);
        base = mul_mod_2_128_host(base, base);
        exp >>= 1;
    }
    return r;
}

__device__ static U128 pow_mod_2_128_dev(U128 base, uint64_t exp) {
    U128 result(1, 0);
    while (exp != 0) {
        if (exp & 1ULL) result = mul_mod_2_128_dev(result, base);
        base = mul_mod_2_128_dev(base, base);
        exp >>= 1;
    }
    return result;
}

// ---------------------------------------------------------------------
// Warp-coalesced kernel.
//
// Grid:  total_threads = total_warps * 32  (multiple of 32 enforced on host)
// Each warp_id covers iters_per_warp * 32 consecutive output positions:
//
//   warp_offset = warp_id * iters_per_warp * 32
//   on iter i, lane l writes out[warp_offset + i*32 + l]
//
// per-lane state evolution: state advances by A^32 each iter.
// per-lane initial jump:    u_init * A^(start_pos + 1).
//
// Numerical equivalence: the value written at out[pos] is exactly
// u_init * A^(pos + 1), matching the scalar reference loop in v1..v4.
// ---------------------------------------------------------------------
__global__ void rng_kernel(double* __restrict__ out,
                           uint64_t   N,
                           uint64_t   iters_per_warp,
                           uint64_t   total_warps,
                           U128       A,
                           U128       A_32,
                           U128       u_init)
{
    const uint64_t tid     = (uint64_t)blockIdx.x * blockDim.x + threadIdx.x;
    const uint64_t warp_id = tid >> 5;
    const uint32_t lane_id = (uint32_t)(tid & 31);

    if (warp_id >= total_warps) return;

    const uint64_t warp_offset = warp_id * iters_per_warp * 32ULL;
    const uint64_t start_pos   = warp_offset + (uint64_t)lane_id;
    if (start_pos >= N) return;

    // Initial jump: state = u_init * A^(start_pos + 1).
    // Cost ~log2(start_pos+1) muls per thread (~30 at N=5e8). The inner
    // loop dwarfs this by orders of magnitude.
    U128 u = u_init;
    {
        const U128 jump = pow_mod_2_128_dev(A, start_pos + 1ULL);
        u = mul_mod_2_128_dev(u, jump);
    }

    constexpr double SCALE = 1.0 / 9007199254740992.0; // 2^-53

    for (uint64_t i = 0; i < iters_per_warp; ++i) {
        const uint64_t pos = warp_offset + i * 32ULL + (uint64_t)lane_id;
        if (pos < N) {
            const uint64_t mantissa = u.hi >> 11;
            // 32 lanes write 32 contiguous doubles --> one coalesced
            // 256-byte transaction to HBM.
            out[pos] = (double)mantissa * SCALE;
        }
        u = mul_mod_2_128_dev(u, A_32);
    }
}

// ---------------------------------------------------------------------
int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: %s <N> <output_file> [block_size]\n", argv[0]);
        return 1;
    }
    const uint64_t N         = std::strtoull(argv[1], nullptr, 10);
    const char*    out_path  = argv[2];
    int            block_sz  = (argc >= 4) ? std::atoi(argv[3]) : 256;
    if (block_sz < 32 || block_sz > 1024 || (block_sz & 31) != 0) {
        std::fprintf(stderr,
                     "[v5] bad block_size=%d, falling back to 256\n", block_sz);
        block_sz = 256;
    }

    int dev_count = 0;
    CUDA_OK(cudaGetDeviceCount(&dev_count));
    if (dev_count <= 0) {
        std::fprintf(stderr, "[v5] no CUDA devices available\n");
        return 4;
    }
    int dev_id = 0;
    CUDA_OK(cudaSetDevice(dev_id));
    cudaDeviceProp prop{};
    CUDA_OK(cudaGetDeviceProperties(&prop, dev_id));
    std::fprintf(stderr,
                 "[v5] GPU: %s  SM %d.%d  SMs=%d  totalMem=%.1f GB\n",
                 prop.name, prop.major, prop.minor,
                 prop.multiProcessorCount,
                 (double)prop.totalGlobalMem / (1ULL << 30));

    // ---- Choose grid geometry ----
    // Want enough threads to saturate occupancy, but not so many that
    // each one's work shrinks below cache-line size.
    //
    // For Ampere/Ada/Hopper the sweet spot is ~8 resident warps per SM
    // for memory-bound kernels (vs. 32 for compute-bound). With 256
    // threads per block that's:
    //
    //   target_max = SMs * 8_blocks_per_sm * 256_threads_per_block
    //              = SMs * 2048 threads
    //
    // For RTX 3090 (82 SMs) that's ~168 000 threads = 5250 warps,
    // each writing ~95k doubles at N=5e8 -- well-coalesced and
    // big enough to amortize launch overhead.
    uint64_t target_max = (uint64_t)prop.multiProcessorCount * 2048ULL;
    uint64_t target_min = 1024ULL;

    uint64_t total_threads = N / 32ULL;          // start: ~one element per lane
    if (total_threads < target_min) total_threads = target_min;
    if (total_threads > target_max) total_threads = target_max;
    if (total_threads > N)          total_threads = N;
    // round up to a multiple of block_sz, and also keep multiple of 32
    total_threads = ((total_threads + block_sz - 1) / block_sz) * block_sz;
    if (total_threads == 0) total_threads = block_sz;

    const uint64_t grid_sz        = total_threads / block_sz;
    const uint64_t total_warps    = total_threads / 32ULL;
    const uint64_t iters_per_warp = (N + total_threads - 1ULL) / total_threads;

    std::fprintf(stderr,
                 "[v5] launch: gridDim=%llu  blockDim=%d  total_threads=%llu  "
                 "warps=%llu  iters_per_warp=%llu\n",
                 (unsigned long long)grid_sz, block_sz,
                 (unsigned long long)total_threads,
                 (unsigned long long)total_warps,
                 (unsigned long long)iters_per_warp);

    double* d_buf = nullptr;
    const size_t bytes = (size_t)N * sizeof(double);
    CUDA_OK(cudaMalloc((void**)&d_buf, bytes));

    // Build A and A^32 on host once.
    const U128 A    = build_A_host();
    const U128 A_32 = pow_mod_2_128_host(A, 32);
    const U128 u0(1, 0);

    cudaEvent_t ev0, ev1;
    CUDA_OK(cudaEventCreate(&ev0));
    CUDA_OK(cudaEventCreate(&ev1));

    CUDA_OK(cudaEventRecord(ev0));
    rng_kernel<<<(unsigned)grid_sz, (unsigned)block_sz>>>(
        d_buf, N, iters_per_warp, total_warps, A, A_32, u0);
    CUDA_OK(cudaGetLastError());
    CUDA_OK(cudaEventRecord(ev1));
    CUDA_OK(cudaEventSynchronize(ev1));

    float kernel_ms = 0.0f;
    CUDA_OK(cudaEventElapsedTime(&kernel_ms, ev0, ev1));

    // Copy back for checksum + dump (NOT timed, same as v1..v4 which
    // also exclude I/O from the time_ms figure).
    std::vector<double> h_buf(N);
    CUDA_OK(cudaMemcpy(h_buf.data(), d_buf, bytes, cudaMemcpyDeviceToHost));

    double checksum = 0.0;
    for (uint64_t i = 0; i < N; ++i) checksum += h_buf[i];

    const uint64_t to_dump = (N < 100ULL) ? N : 100ULL;
    FILE* f = std::fopen(out_path, "w");
    if (!f) { std::printf("cannot open %s\n", out_path); return 2; }
    std::fprintf(f, "# Parallel 128-bit MCG (v5: CUDA, %s SM %d.%d, warp-coalesced)\n",
                 prop.name, prop.major, prop.minor);
    std::fprintf(f, "# total_threads=%llu, block_size=%d, warps=%llu, iters_per_warp=%llu\n",
                 (unsigned long long)total_threads, block_sz,
                 (unsigned long long)total_warps,
                 (unsigned long long)iters_per_warp);
    std::fprintf(f, "# First %llu of %llu generated\n",
                 (unsigned long long)to_dump, (unsigned long long)N);
    std::fprintf(f, "#---------------------------------------------------------\n");
    for (uint64_t i = 0; i < to_dump; ++i) {
        std::fprintf(f, "%6llu\t%.15f\n",
                     (unsigned long long)i, h_buf[(size_t)i]);
    }
    std::fclose(f);

    CUDA_OK(cudaFree(d_buf));
    CUDA_OK(cudaEventDestroy(ev0));
    CUDA_OK(cudaEventDestroy(ev1));

    std::printf("PAR5 N=%llu T=%llu time_ms=%.3f checksum=%.6f\n",
                (unsigned long long)N,
                (unsigned long long)total_threads,
                (double)kernel_ms, checksum);
    return 0;
}
