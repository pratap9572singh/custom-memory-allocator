#pragma once
#include <cstddef>   // for std::size_t
#include <cstdint>   // for std::uintptr_t

// ============================================================================
// BUMP ALLOCATOR (also called an "arena" or "linear" allocator)
// ============================================================================
//
// THE IDEA:
// You grab one big chunk of memory up front. Then every allocation just
// hands out the next slice and moves a pointer forward ("bumps" it).
//
//   buffer:  [####........................]
//                 ^
//                 offset (the bump pointer)
//
//   allocate(8) ->  [########....................]
//                            ^
//
// You CANNOT free individual allocations. You can only reset() the whole
// thing at once. That's the trade-off: insanely fast, zero bookkeeping,
// but no per-object free.
//
// COMPLEXITY:
//   allocate   -> O(1)   (just add to an integer)
//   deallocate -> O(1)   (no-op, does nothing)
//   reset      -> O(1)
//
// WHERE IT'S USED IN REAL LIFE:
//   Game engines (reset the arena every frame), compilers (reset per
//   function), request handling in servers (reset per request).
// ============================================================================

class BumpAllocator {
public:
    // Takes ownership of a raw byte buffer of `capacity` bytes.
    explicit BumpAllocator(std::size_t capacity);
    ~BumpAllocator();

    // No copying an allocator - that would double-free the buffer.
    BumpAllocator(const BumpAllocator&) = delete;
    BumpAllocator& operator=(const BumpAllocator&) = delete;

    // Returns a pointer to `size` bytes, aligned to `alignment`.
    // Returns nullptr if there isn't enough room left.
    void* allocate(std::size_t size, std::size_t alignment = alignof(std::max_align_t));

    // Does nothing. Exists only so the interface matches other allocators.
    void deallocate(void* ptr);

    // Frees EVERYTHING at once by rewinding the bump pointer to zero.
    void reset();

    std::size_t used() const { return m_offset; }
    std::size_t capacity() const { return m_capacity; }
    std::size_t remaining() const { return m_capacity - m_offset; }

private:
    std::byte*  m_buffer;    // the big chunk we hand slices out of
    std::size_t m_capacity;  // total bytes in m_buffer
    std::size_t m_offset;    // how many bytes we've handed out so far
};

// ----------------------------------------------------------------------------
// ALIGNMENT HELPER (used by both allocators)
//
// CPUs want data at addresses that are multiples of the data's size.
// An 8-byte double should live at an address divisible by 8.
// This rounds an address UP to the next multiple of `alignment`.
//
//   align_up(13, 8) -> 16
//   align_up(16, 8) -> 16   (already aligned, don't move)
//
// The bit trick: alignment is always a power of 2 (1,2,4,8,16...).
//   (n + a - 1) & ~(a - 1)
// adds just enough to push past the boundary, then masks off the low bits.
// ----------------------------------------------------------------------------
inline std::size_t align_up(std::size_t n, std::size_t alignment) {
    return (n + alignment - 1) & ~(alignment - 1);
}
