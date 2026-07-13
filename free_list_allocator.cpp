#include "free_list_allocator.h"
#include <new>
#include <cstdio>
#include <cassert>

namespace {
    inline std::size_t align_up(std::size_t n, std::size_t a) {
        return (n + a - 1) & ~(a - 1);
    }
}

// ---------------------------------------------------------------------------
// CONSTRUCTOR
//
// Grab one big chunk from the OS, then lay ONE giant free block across all
// of it. Every block that ever exists is carved out of this original block
// by splitting, and eventually merged back into it by coalescing.
//
//   [ header | ------------- one huge free payload ------------- ]
// ---------------------------------------------------------------------------
FreeListAllocator::FreeListAllocator(std::size_t capacity, FitPolicy policy)
    : m_buffer(static_cast<std::byte*>(::operator new(capacity)))
    , m_capacity(capacity)
    , m_used(0)
    , m_head(nullptr)
    , m_free_head(nullptr)
    , m_policy(policy)
{
    assert(capacity > HEADER_SIZE + MIN_SPLIT_PAYLOAD);

    m_head = reinterpret_cast<BlockHeader*>(m_buffer);
    m_head->size = capacity - HEADER_SIZE;   // everything except the header
    m_head->free = true;
    m_head->next = nullptr;
    m_head->prev = nullptr;
    m_head->next_free = nullptr;
    m_head->prev_free = nullptr;

    m_free_head = m_head;                    // the one block is also the free list
}

FreeListAllocator::~FreeListAllocator() {
    ::operator delete(m_buffer);
}

// ---------------------------------------------------------------------------
// FREE LIST BOOKKEEPING - both O(1)
//
// Standard doubly-linked-list insert/remove at the head. Order doesn't matter
// for correctness, so we always push to the front: cheapest possible insert.
// ---------------------------------------------------------------------------
void FreeListAllocator::free_list_insert(BlockHeader* b) {
    b->prev_free = nullptr;
    b->next_free = m_free_head;
    if (m_free_head) m_free_head->prev_free = b;
    m_free_head = b;
}

void FreeListAllocator::free_list_remove(BlockHeader* b) {
    if (b->prev_free) b->prev_free->next_free = b->next_free;
    else              m_free_head = b->next_free;      // b was the head

    if (b->next_free) b->next_free->prev_free = b->prev_free;

    b->next_free = nullptr;
    b->prev_free = nullptr;
}

// ---------------------------------------------------------------------------
// FIND A FREE BLOCK
//
// Note the loop: `b = b->next_free`, NOT `b->next`.
// We walk ONLY free blocks. Used blocks are invisible to us. This is the
// whole point of keeping the second list.
//
// FirstFit: stop at the first block that fits.       shorter scan
// BestFit:  scan them all, keep the tightest fit.    less wasted space
// ---------------------------------------------------------------------------
FreeListAllocator::BlockHeader*
FreeListAllocator::find_free_block(std::size_t size) const {
    BlockHeader* best = nullptr;

    for (BlockHeader* b = m_free_head; b != nullptr; b = b->next_free) {
        if (b->size < size) continue;             // too small

        if (m_policy == FitPolicy::FirstFit) {
            return b;                             // good enough
        }
        if (best == nullptr || b->size < best->size) {
            best = b;
            if (best->size == size) break;        // perfect fit, can't do better
        }
    }
    return best;   // nullptr = nothing fits
}

// ---------------------------------------------------------------------------
// SPLIT
//
// The block we found is bigger than we need. Cut it in two so the leftover
// stays usable instead of being silently wasted (that waste would be INTERNAL
// FRAGMENTATION - memory inside a block that the owner never asked for).
//
//   before:   [ hdrA | ------------- 200 bytes ------------- ]
//   want 64:  [ hdrA | -- 64 -- ][ hdrB | ---- 120 free ---- ]
//                                          ^ 200 - 64 - HEADER_SIZE
//
// If the leftover is too small to hold a header plus a few usable bytes, we
// DON'T split - we hand over the slightly-too-big block. Splitting would
// create a block nobody could ever allocate from.
//
// PRECONDITION: `block` has already been removed from the free list by the
// caller (allocate). The new leftover block gets inserted into it.
// ---------------------------------------------------------------------------
void FreeListAllocator::split_block(BlockHeader* block, std::size_t size) {
    std::size_t leftover = block->size - size;

    if (leftover < HEADER_SIZE + MIN_SPLIT_PAYLOAD) {
        return;   // not worth splitting
    }

    // The new header goes immediately after our payload.
    std::byte* raw = reinterpret_cast<std::byte*>(block);
    BlockHeader* newBlock =
        reinterpret_cast<BlockHeader*>(raw + HEADER_SIZE + size);

    newBlock->size = leftover - HEADER_SIZE;   // HEADER_SIZE was spent on the header
    newBlock->free = true;
    newBlock->next_free = nullptr;
    newBlock->prev_free = nullptr;

    // Splice into the ADDRESS list, between block and block->next.
    newBlock->next = block->next;
    newBlock->prev = block;
    if (block->next) block->next->prev = newBlock;
    block->next = newBlock;

    // The original block now owns only what the caller asked for.
    block->size = size;

    // The leftover is free, so it joins the FREE list.
    free_list_insert(newBlock);
}

// ---------------------------------------------------------------------------
// ALLOCATE
// ---------------------------------------------------------------------------
void* FreeListAllocator::allocate(std::size_t size, std::size_t alignment) {
    if (size == 0) return nullptr;

    // Payloads always land on ALIGNMENT (16-byte) boundaries because the
    // buffer is 16-aligned, HEADER_SIZE is a multiple of 16, and every size
    // is rounded up to a multiple of 16. Anything STRICTER than that would
    // need extra padding logic, so we honestly refuse it rather than lie.
    if (alignment > ALIGNMENT) return nullptr;

    // Round up so the NEXT header also lands on a boundary.
    size = align_up(size, ALIGNMENT);

    BlockHeader* block = find_free_block(size);
    if (!block) return nullptr;      // out of memory, or too fragmented to serve

    free_list_remove(block);         // it's about to stop being free
    split_block(block, size);        // may or may not actually split
    block->free = false;
    m_used += block->size;

    // Hand back the address JUST PAST the header.
    return reinterpret_cast<std::byte*>(block) + HEADER_SIZE;
}

// ---------------------------------------------------------------------------
// DEALLOCATE  (+ coalescing, inlined here because the two are inseparable)
//
// The user's pointer points at the PAYLOAD. Walk backwards HEADER_SIZE bytes
// to recover the header - the exact mirror image of the last line of
// allocate().
//
// Then merge with free neighbours. Because the ADDRESS list is in memory
// order, block->prev and block->next literally ARE the physical neighbours,
// so a merge is just "absorb their bytes and unlink them".
//
// Note we reclaim the neighbour's HEADER too:
//     merged size = my_size + HEADER_SIZE + neighbour_size
// The header bytes become usable payload again. That's why freeing three
// 64-byte blocks yields 256 free bytes, not 192.
// ---------------------------------------------------------------------------
void FreeListAllocator::deallocate(void* ptr) {
    if (!ptr) return;   // free(nullptr) is legal and does nothing

    std::byte* raw = static_cast<std::byte*>(ptr);
    BlockHeader* block = reinterpret_cast<BlockHeader*>(raw - HEADER_SIZE);

    if (block->free) return;   // double-free guard; real malloc would abort

    m_used -= block->size;
    block->free = true;

    // --- merge with the block to the RIGHT ---------------------------------
    BlockHeader* next = block->next;
    if (next && next->free) {
        free_list_remove(next);                  // it's about to cease to exist
        block->size += HEADER_SIZE + next->size; // swallow its header + payload
        block->next = next->next;                // unlink from the address list
        if (next->next) next->next->prev = block;
    }

    // --- merge with the block to the LEFT ----------------------------------
    BlockHeader* prev = block->prev;
    if (prev && prev->free) {
        // prev is ALREADY in the free list and stays there - we just grow it.
        // `block` was never in the free list (it was allocated), so there is
        // nothing to remove.
        prev->size += HEADER_SIZE + block->size;
        prev->next = block->next;
        if (block->next) block->next->prev = prev;
        return;                                  // prev is the survivor; done
    }

    // No left neighbour to absorb us, so we become a free block in our own
    // right and join the free list.
    free_list_insert(block);
}

// ---------------------------------------------------------------------------
// INTROSPECTION - not part of a real allocator, but invaluable for tests,
// demos, and understanding what's actually happening.
// ---------------------------------------------------------------------------
std::size_t FreeListAllocator::block_count() const {
    std::size_t n = 0;
    for (BlockHeader* b = m_head; b; b = b->next) ++n;
    return n;
}

std::size_t FreeListAllocator::free_block_count() const {
    std::size_t n = 0;
    for (BlockHeader* b = m_free_head; b; b = b->next_free) ++n;
    return n;
}

std::size_t FreeListAllocator::largest_free_block() const {
    std::size_t best = 0;
    for (BlockHeader* b = m_free_head; b; b = b->next_free)
        if (b->size > best) best = b->size;
    return best;
}

// Cross-check the two lists against each other. If they ever disagree, we have
// a bug. Called by the stress test after every few thousand operations.
bool FreeListAllocator::validate() const {
    std::size_t free_via_address = 0;
    std::size_t total_bytes = 0;

    // Walk the ADDRESS list.
    BlockHeader* prev = nullptr;
    for (BlockHeader* b = m_head; b; b = b->next) {
        if (b->prev != prev) return false;              // prev pointers broken
        total_bytes += HEADER_SIZE + b->size;
        if (b->free) ++free_via_address;

        // Two adjacent free blocks must never exist - coalescing failed.
        if (b->free && b->next && b->next->free) return false;

        prev = b;
    }

    // Every byte must be accounted for.
    if (total_bytes != m_capacity) return false;

    // The two lists must agree on how many free blocks there are.
    if (free_via_address != free_block_count()) return false;

    // Every block in the free list must actually be marked free.
    for (BlockHeader* b = m_free_head; b; b = b->next_free)
        if (!b->free) return false;

    return true;
}

void FreeListAllocator::dump() const {
    std::printf("  heap: ");
    for (BlockHeader* b = m_head; b; b = b->next) {
        std::printf("[%s %zu] ", b->free ? "free" : "USED", b->size);
    }
    std::printf("\n         blocks=%zu free_blocks=%zu largest_free=%zu used=%zu\n",
                block_count(), free_block_count(), largest_free_block(), m_used);
}
