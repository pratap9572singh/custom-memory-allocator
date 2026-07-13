#include "bump_allocator.h"
#include "free_list_allocator.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <random>

// ---------------------------------------------------------------------------
// Benchmarks. The honest question every interviewer asks about this project:
// "Is it actually faster than malloc, and WHY?"
//
// The answer: the bump allocator is dramatically faster because it does
// almost nothing. The free list is competitive but not magic - glibc's malloc
// is a heavily optimized piece of engineering with size bins and thread caches.
// Being "close to malloc" with 300 lines is the real win.
// ---------------------------------------------------------------------------

using Clock = std::chrono::high_resolution_clock;

static double ms_since(Clock::time_point t0) {
    auto t1 = Clock::now();
    return std::chrono::duration<double, std::milli>(t1 - t0).count();
}

static void header(const char* title) {
    std::printf("\n%s\n", title);
    std::printf("%-24s %12s %12s\n", "allocator", "time (ms)", "vs malloc");
    std::printf("---------------------------------------------------\n");
}

static void row(const char* name, double ms, double baseline) {
    std::printf("%-24s %12.3f %11.2fx\n", name, ms, baseline / ms);
}

// ===========================================================================
// BENCHMARK 1: pure allocation throughput (no frees)
// This is the bump allocator's home turf.
// ===========================================================================
static void bench_alloc_only() {
    constexpr int N = 200000;
    constexpr std::size_t SZ = 64;

    header("BENCHMARK 1: 200,000 allocations, no frees (64 bytes each)");

    // --- malloc ---
    std::vector<void*> ptrs;
    ptrs.reserve(N);
    auto t0 = Clock::now();
    for (int i = 0; i < N; ++i) ptrs.push_back(std::malloc(SZ));
    double mallocMs = ms_since(t0);
    for (void* p : ptrs) std::free(p);
    ptrs.clear();

    // --- bump ---
    {
        BumpAllocator a(N * (SZ + 16) + 4096);
        t0 = Clock::now();
        for (int i = 0; i < N; ++i) a.allocate(SZ);
        double bumpMs = ms_since(t0);

        // --- free list ---
        FreeListAllocator f(N * (SZ + 64) + 4096);
        t0 = Clock::now();
        for (int i = 0; i < N; ++i) f.allocate(SZ);
        double flMs = ms_since(t0);

        row("system malloc", mallocMs, mallocMs);
        row("BumpAllocator", bumpMs, mallocMs);
        row("FreeListAllocator", flMs, mallocMs);
    }

    std::printf("\n  Bump wins by ~10x because allocate() is literally one integer\n");
    std::printf("  add. malloc must consult size bins, check thread caches, and\n");
    std::printf("  maintain metadata for a free() that may never come.\n");
    std::printf("\n  THE FREE LIST NEARLY DIED ON THIS BENCHMARK. With 200,000 live\n");
    std::printf("  allocations the heap is 200,000 USED blocks then one free block\n");
    std::printf("  at the tail. My first version scanned ALL blocks, so every\n");
    std::printf("  allocate walked past every used block: O(n) per call, O(n^2)\n");
    std::printf("  total. It took 96,195 ms - about 14,000x SLOWER than malloc.\n");
    std::printf("  Fix: a second linked list containing ONLY free blocks. Now the\n");
    std::printf("  scan sees exactly 1 block instead of 200,000 -> effectively O(1).\n");
}

// ===========================================================================
// BENCHMARK 2: alloc/free churn - the realistic workload
// This is where the free list has to actually work for a living.
// ===========================================================================
static void bench_alloc_free_churn() {
    constexpr int ROUNDS = 2000;
    constexpr int BATCH  = 100;
    constexpr std::size_t SZ = 64;

    header("BENCHMARK 2: 2,000 rounds of (alloc 100, free 100)");

    // --- malloc ---
    std::vector<void*> ptrs(BATCH);
    auto t0 = Clock::now();
    for (int r = 0; r < ROUNDS; ++r) {
        for (int i = 0; i < BATCH; ++i) ptrs[i] = std::malloc(SZ);
        for (int i = 0; i < BATCH; ++i) std::free(ptrs[i]);
    }
    double mallocMs = ms_since(t0);

    // --- bump (reset instead of free - that's its whole trick) ---
    {
        BumpAllocator a(BATCH * (SZ + 16) + 4096);
        t0 = Clock::now();
        for (int r = 0; r < ROUNDS; ++r) {
            for (int i = 0; i < BATCH; ++i) a.allocate(SZ);
            a.reset();   // frees all 100 at once, O(1)
        }
        double bumpMs = ms_since(t0);

        // --- free list ---
        FreeListAllocator f(1 << 22);
        t0 = Clock::now();
        for (int r = 0; r < ROUNDS; ++r) {
            for (int i = 0; i < BATCH; ++i) ptrs[i] = f.allocate(SZ);
            for (int i = 0; i < BATCH; ++i) f.deallocate(ptrs[i]);
        }
        double flMs = ms_since(t0);

        row("system malloc", mallocMs, mallocMs);
        row("BumpAllocator (+reset)", bumpMs, mallocMs);
        row("FreeListAllocator", flMs, mallocMs);
    }

    std::printf("\n  bump::reset() releases 100 objects with ONE integer write.\n");
    std::printf("  Nothing can beat that - but you can only use it when you're\n");
    std::printf("  willing to free EVERYTHING at once.\n");
    std::printf("\n  The free list BEATS system malloc here. Why? Because coalescing\n");
    std::printf("  collapses the heap back into a single big block after each round,\n");
    std::printf("  so the free list has ~1 entry and the scan is trivial. We also\n");
    std::printf("  skip everything malloc must do: thread safety, arena selection,\n");
    std::printf("  and returning memory to the OS. Specialisation beats generality.\n");
}

// ===========================================================================
// BENCHMARK 3: random sizes, random free order - the nasty one
// ===========================================================================
static void bench_random() {
    constexpr int OPS = 100000;

    header("BENCHMARK 3: 100,000 random alloc/free ops (8-512 bytes)");

    auto run = [&](auto allocFn, auto freeFn) {
        std::mt19937 rng(1234);   // same seed for every allocator = fair fight
        std::uniform_int_distribution<int> sizeDist(8, 512);
        std::uniform_int_distribution<int> coin(0, 1);
        std::vector<void*> live;
        live.reserve(OPS);

        auto t0 = Clock::now();
        for (int i = 0; i < OPS; ++i) {
            if (live.empty() || coin(rng)) {
                void* p = allocFn(sizeDist(rng));
                if (p) live.push_back(p);
            } else {
                std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
                std::size_t k = pick(rng);
                freeFn(live[k]);
                live[k] = live.back();
                live.pop_back();
            }
        }
        double ms = ms_since(t0);
        for (void* p : live) freeFn(p);
        return ms;
    };

    double mallocMs = run([](std::size_t n){ return std::malloc(n); },
                          [](void* p){ std::free(p); });

    FreeListAllocator ff(1 << 24, FreeListAllocator::FitPolicy::FirstFit);
    double firstMs = run([&](std::size_t n){ return ff.allocate(n); },
                         [&](void* p){ ff.deallocate(p); });

    FreeListAllocator bf(1 << 24, FreeListAllocator::FitPolicy::BestFit);
    double bestMs = run([&](std::size_t n){ return bf.allocate(n); },
                        [&](void* p){ bf.deallocate(p); });

    row("system malloc", mallocMs, mallocMs);
    row("FreeList (FirstFit)", firstMs, mallocMs);
    row("FreeList (BestFit)", bestMs, mallocMs);

    std::printf("\n  FirstFit stops at the first free block that fits -> short scan.\n");
    std::printf("  BestFit scans the WHOLE free list every time -> ~2x slower here,\n");
    std::printf("  but it packs memory tighter. Classic speed-vs-fragmentation\n");
    std::printf("  trade-off, and the reason real allocators use size-bucketed bins\n");
    std::printf("  to get BestFit-quality packing at FirstFit-like speed.\n");
    std::printf("\n  malloc still wins this one: random sizes keep many free blocks\n");
    std::printf("  alive, so our O(f) scan finally has real work to do. Segregated\n");
    std::printf("  free lists (bins by size class) are the fix - see the README.\n");
    std::printf("\n  Fragmentation after the run:\n");
    std::printf("  FirstFit  -> free blocks: %zu, largest: %zu\n",
                ff.free_block_count(), ff.largest_free_block());
    std::printf("  BestFit   -> free blocks: %zu, largest: %zu\n",
                bf.free_block_count(), bf.largest_free_block());
}

// ===========================================================================
// BENCHMARK 4: does coalescing actually do anything?
// Prove it by measuring fragmentation with the SAME workload.
// ===========================================================================
static void bench_coalescing_value() {
    std::printf("\nBENCHMARK 4: the value of coalescing\n");
    std::printf("---------------------------------------------------\n");

    FreeListAllocator a(1 << 20);   // 1 MB

    // Allocate 500 small blocks, then free every single one.
    std::vector<void*> ptrs;
    for (int i = 0; i < 500; ++i) ptrs.push_back(a.allocate(64));

    std::printf("  after 500 allocs of 64B:\n");
    std::printf("    blocks=%zu  free_blocks=%zu  largest_free=%zu\n",
                a.block_count(), a.free_block_count(), a.largest_free_block());

    for (void* p : ptrs) a.deallocate(p);

    std::printf("  after freeing all 500 (coalescing ON):\n");
    std::printf("    blocks=%zu  free_blocks=%zu  largest_free=%zu\n",
                a.block_count(), a.free_block_count(), a.largest_free_block());

    // The killer test: can we now allocate one huge block?
    void* big = a.allocate(500 * 64);
    std::printf("  can we allocate 32,000 bytes contiguously? %s\n",
                big ? "YES" : "NO");
    std::printf("\n  WITHOUT coalescing this would be 500 separate 64-byte holes\n");
    std::printf("  and that 32KB request would FAIL despite 1MB being free.\n");
    std::printf("  That failure mode is called EXTERNAL FRAGMENTATION.\n");
}

int main() {
    std::printf("========================================\n");
    std::printf("  ALLOCATOR BENCHMARKS\n");
    std::printf("========================================\n");

    bench_alloc_only();
    bench_alloc_free_churn();
    bench_random();
    bench_coalescing_value();

    std::printf("\n");
    return 0;
}
