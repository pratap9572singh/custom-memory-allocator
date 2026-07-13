# Custom Memory Allocators in C++

Two allocators written from scratch, with tests and benchmarks against system `malloc`.

- **`BumpAllocator`** — a linear/arena allocator. Allocation is one integer add. You can't free individual objects, only reset the whole arena. ~10× faster than `malloc`.
- **`FreeListAllocator`** — a miniature `malloc`. Block headers, splitting, and coalescing of adjacent free blocks. Supports first-fit and best-fit.

Everything is tested under AddressSanitizer and UBSan. 47/47 tests pass clean.

---

## Build

```bash
make test    # build + run the test suite
make bench   # build + run benchmarks (-O2)
make asan    # run tests under AddressSanitizer + UBSan
```

Requires g++ with C++17. No other dependencies.

---

## How the free list works

Every allocation gets a header stored immediately before the memory the caller receives:

```
+----------------+---------------------------+
|  BlockHeader   |   payload (user's bytes)  |
+----------------+---------------------------+
^                ^
header           the pointer we return
```

Given the user's pointer, `deallocate` recovers the header by walking backwards:

```cpp
BlockHeader* block = (BlockHeader*)((std::byte*)ptr - HEADER_SIZE);
```

That one line is the core trick behind every `malloc` implementation.

### Two linked lists

Each block sits in **two** lists simultaneously:

1. **Address list** (`next`/`prev`) — *all* blocks, in physical memory order.
   Needed for coalescing: a block's list neighbours *are* its memory neighbours.

2. **Free list** (`next_free`/`prev_free`) — *only* free blocks.
   Needed for speed: `allocate` never looks at used blocks.

### Splitting

When a free block is bigger than the request, cut it in two so the remainder stays usable:

```
before:   [free 200]
ask 32:   [used 32][free 120]      120 = 200 - 32 - HEADER_SIZE
```

If the leftover is too small to hold a header plus usable bytes, we don't split — that would create a block nobody could ever allocate from.

### Coalescing

On free, merge with any adjacent free blocks:

```
before:   [free 32][FREED 64][free 16]
after:    [free 256]
```

The merged size is `32 + 64 + 16 + 2 × HEADER_SIZE` — we reclaim the neighbours' headers as usable payload too.

Without coalescing, freeing 500 small blocks leaves 500 unusable holes. You'd have 1 MB free and still fail a 32 KB request. That failure mode is **external fragmentation**, and Benchmark 4 demonstrates it directly.

### Complexity

| Operation | Bump | Free list |
|---|---|---|
| `allocate` | O(1) | O(f), f = number of **free** blocks |
| `deallocate` | O(1) (no-op) | O(1) |
| `reset` | O(1) | — |

---

## The bug the benchmark caught

My first version had only the address list. `allocate` scanned every block from the head.

That's fine when the heap is small. But Benchmark 1 does 200,000 allocations with no frees, which produces a heap of 200,000 **used** blocks followed by one free block at the tail. Every `allocate` walked past every used block to reach it: **O(n) per call, O(n²) overall.**

```
system malloc            6.87 ms
FreeListAllocator   96,195.34 ms     <-- 14,000x slower
```

The fix was to add a second linked list containing only free blocks. Same algorithm, same coalescing logic, one extra pointer pair in the header — and the scan now sees 1 block instead of 200,000:

```
system malloc            6.53 ms
FreeListAllocator        7.30 ms     <-- 13,000x faster than before
```

This is why the benchmarks exist. The tests all passed on the broken version — correctness and performance are different questions.

---

## Benchmark results

Measured on Linux, g++ `-O2`.

**1. 200,000 allocations, no frees (64 B each)**

| allocator | time | vs malloc |
|---|---|---|
| system malloc | 6.53 ms | 1.00× |
| BumpAllocator | 0.60 ms | **10.93×** |
| FreeListAllocator | 7.30 ms | 0.89× |

Bump wins because `allocate` is one integer add. `malloc` maintains metadata for a `free()` that may never come.

**2. 2,000 rounds of (alloc 100, free 100)**

| allocator | time | vs malloc |
|---|---|---|
| system malloc | 3.01 ms | 1.00× |
| BumpAllocator (+reset) | 0.44 ms | **6.84×** |
| FreeListAllocator | 1.49 ms | **2.03×** |

The free list beats `malloc` here: coalescing collapses the heap back to one block each round, so the free list has ~1 entry. We also skip thread safety and arena selection entirely. Specialisation beats generality.

**3. 100,000 random alloc/free ops (8–512 B)**

| allocator | time | vs malloc |
|---|---|---|
| system malloc | 2.33 ms | 1.00× |
| FreeList (FirstFit) | 3.36 ms | 0.69× |
| FreeList (BestFit) | 6.48 ms | 0.36× |

`malloc` wins this one. Random sizes keep many free blocks alive, so the O(f) scan finally has real work to do. BestFit is ~2× slower than FirstFit because it scans the entire free list every time — the classic speed-vs-fragmentation trade-off.

**4. Coalescing**

Allocate 500 × 64 B, then free all of them. With coalescing on, the heap collapses from 501 blocks back to 1, and a subsequent 32,000-byte contiguous allocation **succeeds**. Without it, that request would fail despite 1 MB being free.

---

## Testing

47 assertions across both allocators:

- alignment (payloads land on `max_align_t` boundaries for awkward sizes)
- exhaustion (`nullptr` on OOM, never overruns the buffer)
- splitting and block reuse
- coalescing left, right, and **both sides at once** (the triple merge)
- worst-case fragmentation (free every other block, then verify full recovery)
- BestFit picks the exact-fit hole and leaves large holes intact
- 20,000-operation randomized stress test with:
  - **byte-pattern verification** — each allocation is filled with a known pattern and re-checked before free, so any overlapping blocks are caught
  - **invariant validation** — cross-checks that the two lists agree, all bytes are accounted for, and no two free blocks are ever adjacent (which would mean coalescing silently failed)

All clean under `-fsanitize=address,undefined`.

---

## What's missing (honest limitations)

- **Not thread-safe.** No locks. Real `malloc` has per-thread arenas.
- **Alignment capped at `alignof(std::max_align_t)`.** Stricter alignment (e.g. 64-byte for SIMD) returns `nullptr` rather than lying about it.
- **Fixed capacity.** Can't grow by requesting more from the OS (`sbrk`/`mmap`).
- **`allocate` is O(f), not O(1).** The next upgrade is **segregated free lists**: separate free lists bucketed by size class (16 B, 32 B, 64 B…), so you jump straight to a list where every block is guaranteed to fit. That's what makes real `malloc` fast, and it's the single biggest remaining win here.
- No `realloc`, no `calloc`, no `std::pmr` integration.

## Files

```
include/bump_allocator.h        interface + design notes
include/free_list_allocator.h   interface + design notes
src/bump_allocator.cpp
src/free_list_allocator.cpp     the interesting one
tests/test_allocators.cpp       47 assertions
benchmarks/benchmark.cpp        4 benchmarks vs malloc
Makefile
```
