#include "bump_allocator.h"
#include <new>       // for ::operator new / ::operator delete

// Constructor: ask the OS (via global new) for one big raw chunk of bytes.
// This is the ONLY time we touch the system allocator. Everything after
// this is served out of our own buffer.
BumpAllocator::BumpAllocator(std::size_t capacity)
    : m_buffer(static_cast<std::byte*>(::operator new(capacity)))
    , m_capacity(capacity)
    , m_offset(0)
{
}

// Destructor: give the one big chunk back.
BumpAllocator::~BumpAllocator() {
    ::operator delete(m_buffer);
}

void* BumpAllocator::allocate(std::size_t size, std::size_t alignment) {
    // Step 1: Where would the next allocation naturally start?
    //         (as an absolute address, not just an offset)
    std::uintptr_t current = reinterpret_cast<std::uintptr_t>(m_buffer) + m_offset;

    // Step 2: Round that address UP to a properly aligned address.
    std::uintptr_t aligned = align_up(current, alignment);

    // Step 3: How many bytes did we waste by rounding up? That's padding.
    std::size_t padding = aligned - current;

    // Step 4: Do we actually have room for (padding + the request)?
    //         Note we check this BEFORE bumping - never bump past the end.
    if (m_offset + padding + size > m_capacity) {
        return nullptr;   // out of memory in this arena
    }

    // Step 5: Bump the pointer forward and hand back the aligned address.
    m_offset += padding + size;
    return reinterpret_cast<void*>(aligned);
}

// A bump allocator physically cannot free one object. To free block #3 you
// would have to move blocks #4..#N backwards, and every pointer anyone holds
// to them would become garbage. So: no-op.
void BumpAllocator::deallocate(void* /*ptr*/) {
    // intentionally empty
}

// Free everything in O(1) by pretending we never allocated anything.
// The bytes aren't wiped - they're just marked as available again.
void BumpAllocator::reset() {
    m_offset = 0;
}
