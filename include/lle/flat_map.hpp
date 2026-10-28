#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace lle {

// Fibonacci hashing: multiply and keep the top bits, which mixes sequential keys well.
struct FibonacciHash {
    static std::size_t slot(std::uint64_t key, unsigned shift) noexcept {
        return static_cast<std::size_t>((key * 0x9e3779b97f4a7c15ull) >> shift);
    }
};

// Open addressing, linear probing, fixed power-of-two capacity, backward-shift deletion so
// there are no tombstones. Key 0 marks an empty slot and cannot be stored.
template <class K, class V, class Hash = FibonacciHash>
class FlatMap {
    struct Slot {
        K key;
        V value;
    };

public:
    explicit FlatMap(std::size_t capacity)
        : slots_(new Slot[capacity]()),
          mask_(capacity - 1),
          shift_(static_cast<unsigned>(64 - std::countr_zero(capacity))) {}

    std::size_t size() const noexcept { return size_; }
    std::size_t capacity() const noexcept { return mask_ + 1; }

    V* find(K key) noexcept {
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            Slot& s = slots_[i];
            if (s.key == key) return &s.value;
            if (s.key == 0) return nullptr;
        }
    }
    const V* find(K key) const noexcept { return const_cast<FlatMap*>(this)->find(key); }

    // False if the key is already present. The caller keeps size below capacity.
    bool insert(K key, V value) noexcept {
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            Slot& s = slots_[i];
            if (s.key == key) return false;
            if (s.key == 0) {
                s.key = key;
                s.value = value;
                ++size_;
                return true;
            }
        }
    }

    bool erase(K key) noexcept {
        std::size_t i = home(key);
        for (;; i = (i + 1) & mask_) {
            if (slots_[i].key == key) break;
            if (slots_[i].key == 0) return false;
        }
        // Pull later members of the probe run back into the hole when the hole lies
        // cyclically within [their home, their slot).
        for (std::size_t j = (i + 1) & mask_; slots_[j].key != 0; j = (j + 1) & mask_) {
            std::size_t h = home(slots_[j].key);
            if (((j - h) & mask_) >= ((j - i) & mask_)) {
                slots_[i] = slots_[j];
                i = j;
            }
        }
        slots_[i].key = 0;
        --size_;
        return true;
    }

private:
    std::size_t home(K key) const noexcept { return Hash::slot(key, shift_) & mask_; }

    std::unique_ptr<Slot[]> slots_;
    std::size_t mask_;
    unsigned shift_;
    std::size_t size_ = 0;
};

}  // namespace lle
