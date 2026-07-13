#pragma once
#include <cstddef>
#include <cstdint>

// ============================================================================
// FREE LIST ALLOCATOR (a real malloc, in miniature)
// ============================================================================
//
// THE IDEA:
// The buffer is carved into BLOCKS. Every block has a small HEADER sitting
// directly in front of the memory the user gets. The header records how big
// the block is and whether it's free.
//
// Memory layout of one block:
//
//     +----------------+---------------------------+
//     |  BlockHeader   |   payload (user's bytes)  |
//     +----------------+---------------------------+
//     ^                ^
//     header           this is the pointer we return to the caller
//
// So given the user's pointer, we recover the header by walking BACKWARDS:
//     header = (BlockHeader*)((std::byte*)ptr - HEADER_SIZE)
// That single trick is the heart of every malloc implementation.
//
// ----------------------------------------------------------------------------
// THE TWO LISTS  <- the key design decision in this file
// ----------------------------------------------------------------------------
//
// Every block lives in TWO linked lists at once:
//
// 1. The ADDRESS LIST (next / prev)
//    ALL blocks, used and free, in physical memory order.
//
//        [used 32] <-> [free 64] <-> [free 16] <-> [used 128]
//
//    WHY: when a block is freed, its list neighbours ARE its physical
//    neighbours in memory. That's the only reason COALESCING is possible.
//
// 2. The FREE LIST (next_free / prev_free)
//    ONLY free blocks. Order doesn't matter.
//
//        free_head -> [free 64] <-> [free 16]
//
//    WHY: so allocate() never wastes a single cycle looking at used blocks.
//
// I FIRST WROTE THIS WITH ONLY LIST #1, and the benchmark destroyed it.
// With 200,000 live allocations the layout is 200,000 USED blocks followed
// by one free block at the tail. Scanning from the head meant walking past
// every used block on EVERY allocate -> O(n) per call, O(n^2) overall.
// It took 96 SECONDS where system malloc took 7 milliseconds.
//
// Adding list #2 means allocate() only ever touches free blocks. In that same
// benchmark there is exactly ONE free block, so the scan becomes O(1).
// Same algorithm, same coalescing - one extra pointer pair in the header.
//
// ----------------------------------------------------------------------------
// THE THREE OPERATIONS
// ----------------------------------------------------------------------------
//
// 1. ALLOCATE - walk the FREE list, find a block big enough.
//               If it's much bigger than needed, SPLIT it.
//
//    before:   [free 128]
//    ask 32:   [used 32][free 80]        (80 = 128 - 32 - HEADER_SIZE)
//
// 2. DEALLOCATE - flip the free flag, then COALESCE.
//
// 3. COALESCE - merge with free neighbours so we don't accumulate a thousand
//               useless little holes.
//
//    before:   [free 32][FREED 64][free 16]
//    after:    [free 128]      <- 32 + 64 + 16 + 2 reclaimed headers
//
//    Without this you can have tons of free memory and still fail a 100-byte
//    request because it's scattered into unusable fragments. That failure is
//    called EXTERNAL FRAGMENTATION.
//
// ----------------------------------------------------------------------------
// COMPLEXITY
// ----------------------------------------------------------------------------
//   allocate   -> O(f), f = number of FREE blocks   (NOT total blocks)
//   deallocate -> O(1)  flip a flag, merge 2 neighbours, splice the lists
//   coalesce   -> O(1)  only ever touches prev and next
//
//   Real malloc gets allocate to ~O(1) with SEGREGATED FREE LISTS: separate
//   free lists bucketed by size class, so it jumps straight to a list where
//   everything is guaranteed to fit. That's the natural next upgrade.
// ============================================================================

class FreeListAllocator {
public:
    // Which free block do we pick when several of them fit?
    enum class FitPolicy {
        FirstFit,   // first one that fits    -> fast, more fragmentation
        BestFit     // smallest one that fits -> slower, packs tighter
    };

    explicit FreeListAllocator(std::size_t capacity,
                               FitPolicy policy = FitPolicy::FirstFit);
    ~FreeListAllocator();

    FreeListAllocator(const FreeListAllocator&) = delete;
    FreeListAllocator& operator=(const FreeListAllocator&) = delete;

    void* allocate(std::size_t size, std::size_t alignment = alignof(std::max_align_t));
    void  deallocate(void* ptr);

    // --- introspection: used by the tests and benchmarks -------------------
    std::size_t used() const     { return m_used; }
    std::size_t capacity() const { return m_capacity; }
    std::size_t block_count() const;         // total blocks (walks address list)
    std::size_t free_block_count() const;    // walks the free list
    std::size_t largest_free_block() const;  // biggest single alloc still possible
    void        dump() const;                // print a map of the whole heap
    bool        validate() const;            // assert the two lists agree

private:
    struct BlockHeader {
        std::size_t  size;        // PAYLOAD bytes (does not include this header)
        bool         free;

        BlockHeader* next;        // address list: physically to the right
        BlockHeader* prev;        // address list: physically to the left

        BlockHeader* next_free;   // free list: only meaningful when free == true
        BlockHeader* prev_free;   // free list: only meaningful when free == true
    };

    static constexpr std::size_t ALIGNMENT   = alignof(std::max_align_t);
    static constexpr std::size_t HEADER_SIZE =
        (sizeof(BlockHeader) + ALIGNMENT - 1) & ~(ALIGNMENT - 1);

    static constexpr std::size_t MIN_SPLIT_PAYLOAD = ALIGNMENT;

    BlockHeader* find_free_block(std::size_t size) const;
    void         split_block(BlockHeader* block, std::size_t size);

    void free_list_insert(BlockHeader* b);   // O(1)
    void free_list_remove(BlockHeader* b);   // O(1)

    std::byte*   m_buffer;
    std::size_t  m_capacity;
    std::size_t  m_used;
    BlockHeader* m_head;        // address list head (lowest address)
    BlockHeader* m_free_head;   // free list head
    FitPolicy    m_policy;
};
