// Decides whether a C++ allocation belongs in the game's JKRHeap tree or in the host heap.
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// True when the calling thread is an emulated game thread (OSThread) that is not
// inside a PortHostAllocScope.
int port_use_game_heap(void);

// True when p lies inside the emulated MEM1/MEM2 arenas that back the JKR heaps.
int port_is_game_heap_ptr(const void* p);

void* port_host_alloc(size_t size, int align);
void port_host_free(void* p);

void port_host_alloc_scope_enter(void);
void port_host_alloc_scope_leave(void);

#ifdef __cplusplus
}

// RAII guard for platform code that runs on a game thread but must allocate from
// the host heap (std containers inside the renderer, file system, etc).
struct PortHostAllocScope {
    PortHostAllocScope() { port_host_alloc_scope_enter(); }
    ~PortHostAllocScope() { port_host_alloc_scope_leave(); }
    PortHostAllocScope(const PortHostAllocScope&) = delete;
    PortHostAllocScope& operator=(const PortHostAllocScope&) = delete;
};

// Allocator for containers shared between game and host threads: always the
// host heap, whichever thread grows or shrinks them (a game heap block freed
// on a host thread would take the heap's OS mutex without an OS thread).
template <class T>
struct PortHostAllocator {
    typedef T value_type;
    PortHostAllocator() noexcept {}
    template <class U>
    PortHostAllocator(const PortHostAllocator<U>&) noexcept {}
    T* allocate(size_t n) { return (T*)port_host_alloc(n * sizeof(T), (int)alignof(T)); }
    void deallocate(T* p, size_t) noexcept { port_host_free(p); }
    template <class U>
    bool operator==(const PortHostAllocator<U>&) const noexcept { return true; }
    template <class U>
    bool operator!=(const PortHostAllocator<U>&) const noexcept { return false; }
};
#endif
