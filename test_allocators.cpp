#include "bump_allocator.h"
#include "free_list_allocator.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <vector>
#include <random>

// ---------------------------------------------------------------------------
// A dead-simple test framework. No dependencies, no CMake, no gtest.
// ---------------------------------------------------------------------------
static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg)                                              \
    do {                                                              \
        if (cond) { ++g_pass; }                                       \
        else {                                                        \
            ++g_fail;                                                 \
            std::printf("  FAIL [%s:%d] %s\n", __FILE__, __LINE__, msg); \
        }                                                             \
    } while (0)

static void section(const char* name) {
    std::printf("\n=== %s ===\n", name);
}

// ===========================================================================
// BUMP ALLOCATOR TESTS
// ===========================================================================
static void test_bump_basic() {
    section("Bump: basic allocation");

    BumpAllocator a(1024);
    CHECK(a.used() == 0, "starts empty");

    void* p1 = a.allocate(64);
    CHECK(p1 != nullptr, "first allocation succeeds");
    CHECK(a.used() >= 64, "used() grew");

    void* p2 = a.allocate(64);
    CHECK(p2 != nullptr, "second allocation succeeds");
    CHECK(p1 != p2, "two allocations return different pointers");

    // p2 must come AFTER p1 in memory - that's the whole point of "bump".
    CHECK(static_cast<std::byte*>(p2) > static_cast<std::byte*>(p1),
          "allocations move forward");
}

static void test_bump_alignment() {
    section("Bump: alignment");

    BumpAllocator a(1024);

    // Ask for a deliberately awkward size to knock the offset off-boundary.
    a.allocate(1);
    void* p = a.allocate(8, 8);
    auto addr = reinterpret_cast<std::uintptr_t>(p);
    CHECK(addr % 8 == 0, "8-byte aligned");

    a.allocate(3);
    void* q = a.allocate(16, 16);
    CHECK(reinterpret_cast<std::uintptr_t>(q) % 16 == 0, "16-byte aligned");
}

static void test_bump_exhaustion() {
    section("Bump: running out of memory");

    BumpAllocator a(128);
    void* p = a.allocate(100);
    CHECK(p != nullptr, "fits");

    void* q = a.allocate(100);
    CHECK(q == nullptr, "returns nullptr when full (does NOT overrun buffer)");
}

static void test_bump_reset() {
    section("Bump: reset");

    BumpAllocator a(1024);
    void* p1 = a.allocate(512);
    a.reset();
    CHECK(a.used() == 0, "reset clears used()");

    void* p2 = a.allocate(512);
    CHECK(p1 == p2, "after reset we hand out the same address again");
}

static void test_bump_writable() {
    section("Bump: memory is actually usable");

    BumpAllocator a(1024);
    int* nums = static_cast<int*>(a.allocate(sizeof(int) * 10, alignof(int)));
    for (int i = 0; i < 10; ++i) nums[i] = i * i;

    bool ok = true;
    for (int i = 0; i < 10; ++i) if (nums[i] != i * i) ok = false;
    CHECK(ok, "wrote and read back 10 ints correctly");
}

// ===========================================================================
// FREE LIST ALLOCATOR TESTS
// ===========================================================================
static void test_fl_basic() {
    section("FreeList: basic allocation");

    FreeListAllocator a(4096);
    CHECK(a.block_count() == 1, "starts as one big block");
    CHECK(a.free_block_count() == 1, "that block is free");

    void* p = a.allocate(64);
    CHECK(p != nullptr, "allocation succeeds");
    CHECK(a.block_count() == 2, "block was split into [used][free]");
    CHECK(a.free_block_count() == 1, "one free block remains");

    a.deallocate(p);
    CHECK(a.free_block_count() >= 1, "freed");
}

static void test_fl_reuse() {
    section("FreeList: freed memory is reused");

    FreeListAllocator a(4096);

    void* p1 = a.allocate(64);
    a.deallocate(p1);
    void* p2 = a.allocate(64);

    CHECK(p1 == p2, "same slot handed out again after free");
}

static void test_fl_split() {
    section("FreeList: splitting");

    FreeListAllocator a(4096);

    void* p1 = a.allocate(32);
    void* p2 = a.allocate(32);
    void* p3 = a.allocate(32);

    CHECK(a.block_count() == 4, "3 used blocks + 1 remaining free block");
    CHECK(p1 && p2 && p3, "all three succeeded");

    // Each must sit strictly after the previous, with room for a header.
    auto d12 = static_cast<std::byte*>(p2) - static_cast<std::byte*>(p1);
    CHECK(d12 >= 32, "blocks don't overlap");
}

static void test_fl_coalesce_right() {
    section("FreeList: coalesce with RIGHT neighbour");

    FreeListAllocator a(4096);

    void* p1 = a.allocate(64);
    void* p2 = a.allocate(64);
    a.allocate(64);   // p3 - keep it allocated so we don't merge into the tail

    std::size_t before = a.block_count();

    a.deallocate(p2);   // free the middle
    a.deallocate(p1);   // free the left -> p1 should absorb p2

    // p1 and p2 merged into a single free block, so we lost one block.
    CHECK(a.block_count() == before - 1, "two adjacent frees became one block");
    CHECK(a.largest_free_block() >= 64 + 64, "merged block holds both payloads + header");
}

static void test_fl_coalesce_both_sides() {
    section("FreeList: coalesce with BOTH neighbours");

    FreeListAllocator a(4096);

    void* p1 = a.allocate(64);
    void* p2 = a.allocate(64);
    void* p3 = a.allocate(64);
    void* p4 = a.allocate(64);   // guard so the tail free block stays separate
    (void)p4;

    std::printf("  after 4 allocs:\n");
    a.dump();

    a.deallocate(p1);   // free left
    a.deallocate(p3);   // free right
    std::printf("  after freeing p1 and p3 (p2 still used, holes on both sides):\n");
    a.dump();

    std::size_t before = a.block_count();

    a.deallocate(p2);   // free the middle -> must merge LEFT and RIGHT at once
    std::printf("  after freeing p2 (triple merge):\n");
    a.dump();

    // Three blocks became one -> we lost two blocks.
    CHECK(a.block_count() == before - 2, "three free blocks merged into one");

    // The merged block must be big enough for all 3 payloads plus 2 reclaimed
    // headers - proving we didn't leak the headers.
    CHECK(a.largest_free_block() >= 64 * 3, "merged payload spans all three");
}

static void test_fl_fragmentation() {
    section("FreeList: fragmentation, and why coalescing matters");

    FreeListAllocator a(4096);

    // Allocate a run of small blocks, then free EVERY OTHER one.
    // This is the classic fragmentation pattern.
    std::vector<void*> ptrs;
    for (int i = 0; i < 16; ++i) ptrs.push_back(a.allocate(64));

    for (int i = 0; i < 16; i += 2) a.deallocate(ptrs[i]);

    std::printf("  freed every other block (worst-case fragmentation):\n");
    a.dump();
    std::size_t frag_largest = a.largest_free_block();

    // Now free the rest. Coalescing should collapse everything back down.
    for (int i = 1; i < 16; i += 2) a.deallocate(ptrs[i]);

    std::printf("  after freeing everything (should collapse to 1 block):\n");
    a.dump();

    CHECK(a.block_count() == 1, "everything coalesced back into one block");
    CHECK(a.used() == 0, "nothing is in use");
    CHECK(a.largest_free_block() > frag_largest,
          "coalescing recovered a much bigger contiguous block");
}

static void test_fl_alignment() {
    section("FreeList: alignment");

    FreeListAllocator a(4096);
    for (int i = 0; i < 8; ++i) {
        void* p = a.allocate(1 + i * 7);   // awkward sizes
        CHECK(reinterpret_cast<std::uintptr_t>(p) % alignof(std::max_align_t) == 0,
              "payload is max_align_t aligned");
    }
}

static void test_fl_exhaustion() {
    section("FreeList: out of memory");

    FreeListAllocator a(256);
    void* p = a.allocate(1000);
    CHECK(p == nullptr, "oversized request returns nullptr");

    CHECK(a.allocate(0) == nullptr, "zero-size request returns nullptr");
    a.deallocate(nullptr);   // must not crash
    CHECK(true, "deallocate(nullptr) is safe");
}

static void test_fl_stress() {
    section("FreeList: randomized stress test");

    FreeListAllocator a(1 << 20);   // 1 MB
    std::mt19937 rng(42);           // fixed seed -> reproducible
    std::uniform_int_distribution<int> sizeDist(8, 512);
    std::uniform_int_distribution<int> coin(0, 1);

    std::vector<std::pair<void*, std::size_t>> live;
    bool corrupted = false;
    bool invalid   = false;

    for (int step = 0; step < 20000; ++step) {
        // Periodically cross-check that the address list and the free list
        // still agree with each other, and that no two free blocks are
        // adjacent (which would mean coalescing silently failed).
        if (step % 500 == 0 && !a.validate()) invalid = true;

        if (live.empty() || coin(rng)) {
            std::size_t n = sizeDist(rng);
            void* p = a.allocate(n);
            if (p) {
                // Fill with a recognizable byte pattern so we can detect if a
                // later allocation stomps on it (that would mean overlapping
                // blocks - the worst possible allocator bug).
                std::memset(p, static_cast<int>(n & 0xFF), n);
                live.emplace_back(p, n);
            }
        } else {
            std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
            std::size_t i = pick(rng);

            // Verify our pattern survived before we free it.
            auto [ptr, n] = live[i];
            auto* bytes = static_cast<unsigned char*>(ptr);
            for (std::size_t k = 0; k < n; ++k) {
                if (bytes[k] != static_cast<unsigned char>(n & 0xFF)) {
                    corrupted = true;
                }
            }
            a.deallocate(ptr);
            live[i] = live.back();
            live.pop_back();
        }
    }

    CHECK(!corrupted, "no allocation ever overlapped another (20,000 random ops)");
    CHECK(!invalid,   "address list and free list stayed consistent throughout");

    // Drain everything - the heap must collapse back to a single free block.
    for (auto& [ptr, n] : live) a.deallocate(ptr);

    std::printf("  final state after draining:\n");
    a.dump();
    CHECK(a.used() == 0, "all memory returned");
    CHECK(a.block_count() == 1, "heap fully coalesced back to one block");
}

static void test_fl_best_fit() {
    section("FreeList: BestFit vs FirstFit");

    FreeListAllocator a(4096, FreeListAllocator::FitPolicy::BestFit);

    // Carve three holes of different sizes: 256, 32, 128
    void* p1 = a.allocate(256);
    void* g1 = a.allocate(16);     // guards prevent the holes from merging
    void* p2 = a.allocate(32);
    void* g2 = a.allocate(16);
    void* p3 = a.allocate(128);
    void* g3 = a.allocate(16);
    (void)g1; (void)g2; (void)g3;

    a.deallocate(p1);   // hole of 256
    a.deallocate(p2);   // hole of 32
    a.deallocate(p3);   // hole of 128

    std::printf("  three holes: 256, 32, 128\n");
    a.dump();

    // Ask for 32. BestFit must pick the 32-byte hole (a perfect fit),
    // NOT the 256-byte hole that FirstFit would grab.
    void* q = a.allocate(32);
    std::printf("  after allocating 32 (BestFit should take the exact-fit hole):\n");
    a.dump();

    CHECK(q == p2, "BestFit chose the exact-fit hole, leaving big holes intact");
    CHECK(a.largest_free_block() >= 256, "the 256 hole is still whole");
}

int main() {
    std::printf("========================================\n");
    std::printf("  CUSTOM ALLOCATOR TEST SUITE\n");
    std::printf("========================================\n");

    test_bump_basic();
    test_bump_alignment();
    test_bump_exhaustion();
    test_bump_reset();
    test_bump_writable();

    test_fl_basic();
    test_fl_reuse();
    test_fl_split();
    test_fl_coalesce_right();
    test_fl_coalesce_both_sides();
    test_fl_fragmentation();
    test_fl_alignment();
    test_fl_exhaustion();
    test_fl_best_fit();
    test_fl_stress();

    std::printf("\n========================================\n");
    std::printf("  passed: %d   failed: %d\n", g_pass, g_fail);
    std::printf("========================================\n");
    return g_fail == 0 ? 0 : 1;
}
