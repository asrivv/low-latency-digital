#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

#include "lle/common.hpp"

namespace lle {

// Bounded single-producer single-consumer ring. Indices grow monotonically and are masked
// on access. Each side keeps a private copy of the other side's index on its own cache line
// and reloads the shared one only when the copy says full (producer) or empty (consumer).
template <class T, std::size_t Capacity>
class SpscRing {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");
    static constexpr std::uint64_t kMask = Capacity - 1;

public:
    SpscRing() : slots_(new T[Capacity]()) {}
    SpscRing(const SpscRing&) = delete;
    SpscRing& operator=(const SpscRing&) = delete;

    static constexpr std::size_t capacity() noexcept { return Capacity; }

    // Producer: slot to fill in place, or nullptr when full. Publish with commit().
    T* claim() noexcept {
        std::uint64_t t = tail_.load(std::memory_order_relaxed);
        if (t - head_cache_ == Capacity) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (t - head_cache_ == Capacity) return nullptr;
        }
        return &slots_[t & kMask];
    }

    // Release: the slot contents written after claim() become visible before the new tail.
    void commit() noexcept { tail_.store(tail_.load(std::memory_order_relaxed) + 1, std::memory_order_release); }

    bool try_push(const T& v) noexcept {
        T* s = claim();
        if (s == nullptr) return false;
        *s = v;
        commit();
        return true;
    }

    // Consumer: oldest slot, or nullptr when empty. Hand it back with release().
    T* peek() noexcept {
        std::uint64_t h = head_.load(std::memory_order_relaxed);
        if (h == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (h == tail_cache_) return nullptr;
        }
        return &slots_[h & kMask];
    }

    // Consumer: the slot k positions after the oldest, or nullptr if it is not published yet.
    T* peek_at(std::size_t k) noexcept {
        std::uint64_t i = head_.load(std::memory_order_relaxed) + k;
        if (i >= tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (i >= tail_cache_) return nullptr;
        }
        return &slots_[i & kMask];
    }

    // Release: the producer must not overwrite the slot until our reads of it are done.
    void release() noexcept { head_.store(head_.load(std::memory_order_relaxed) + 1, std::memory_order_release); }

    bool try_pop(T& out) noexcept {
        T* s = peek();
        if (s == nullptr) return false;
        out = *s;
        release();
        return true;
    }

    bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

private:
    alignas(kCacheLine) std::atomic<std::uint64_t> tail_{0};
    std::uint64_t head_cache_ = 0;
    alignas(kCacheLine) std::atomic<std::uint64_t> head_{0};
    std::uint64_t tail_cache_ = 0;
    alignas(kCacheLine) std::unique_ptr<T[]> slots_;
};

}  // namespace lle
