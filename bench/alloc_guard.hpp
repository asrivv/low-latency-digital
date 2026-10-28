#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>

// Counting replacement of the global allocation functions. Include from exactly one
// translation unit per benchmark binary.
namespace lle::bench {
inline std::atomic<std::uint64_t> g_allocations{0};
inline std::uint64_t allocations() noexcept { return g_allocations.load(std::memory_order_relaxed); }
}  // namespace lle::bench

namespace {
void* counted_alloc(std::size_t n, std::size_t align = 0) noexcept {
    lle::bench::g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (n == 0) n = 1;
    void* p = nullptr;
    if (align > alignof(std::max_align_t)) {
        if (posix_memalign(&p, align, n) != 0) p = nullptr;
    } else {
        p = std::malloc(n);
    }
    if (p == nullptr) std::abort();
    return p;
}
}  // namespace

void* operator new(std::size_t n) { return counted_alloc(n); }
void* operator new[](std::size_t n) { return counted_alloc(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept { return counted_alloc(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return counted_alloc(n); }
void* operator new(std::size_t n, std::align_val_t a) { return counted_alloc(n, static_cast<std::size_t>(a)); }
void* operator new[](std::size_t n, std::align_val_t a) { return counted_alloc(n, static_cast<std::size_t>(a)); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
