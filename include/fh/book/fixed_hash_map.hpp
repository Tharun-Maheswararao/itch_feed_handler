// Open-addressing map from a 64-bit key to a 32-bit value with a FIXED
// capacity. Same probing and deletion scheme as Version 1's OrderMap
// (linear probing, Fibonacci hashing, backward-shift deletion), but it never
// grows: inserting past the load limit fails, and the caller counts that as
// a capacity error. Growth would mean a rehash, an allocation and a latency
// spike on the hot path, which Version 2 forbids.
//
// Key 0 is stored out of line, so every 64-bit key is valid.
#pragma once

#include <cstdint>
#include <stdexcept>
#include <vector>

namespace fh {

class FixedHashMap {
public:
    // capacity_pow2 slots; at most max_load_percent of them may be used.
    explicit FixedHashMap(std::size_t capacity_pow2, unsigned max_load_percent = 75)
        : slots_(capacity_pow2), mask_(capacity_pow2 - 1),
          limit_(capacity_pow2 * max_load_percent / 100) {
        if (capacity_pow2 < 16 || (capacity_pow2 & mask_) != 0)
            throw std::invalid_argument("FixedHashMap capacity must be a power of two >= 16");
        shift_ = 64 - __builtin_ctzll(capacity_pow2);
    }

    // Pointer to the value for `key`, or nullptr.
    uint32_t* find(uint64_t key) noexcept {
        if (key == 0) return zero_used_ ? &zero_value_ : nullptr;
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            Slot& s = slots_[i];
            if (s.key == key) return &s.value;
            if (s.key == 0) return nullptr;
        }
    }

    // Inserts `key` (value 0) and returns its value slot. `inserted` is false
    // if it was already present. Returns nullptr when the map is full.
    uint32_t* insert(uint64_t key, bool& inserted) noexcept {
        if (key == 0) {
            inserted = !zero_used_;
            if (inserted) zero_value_ = 0;
            zero_used_ = true;
            return &zero_value_;
        }
        for (std::size_t i = home(key);; i = (i + 1) & mask_) {
            Slot& s = slots_[i];
            if (s.key == key) {
                inserted = false;
                return &s.value;
            }
            if (s.key == 0) {
                if (size_ >= limit_) return nullptr;
                s.key = key;
                s.value = 0;
                ++size_;
                inserted = true;
                return &s.value;
            }
        }
    }

    bool erase(uint64_t key) noexcept {
        if (key == 0) {
            const bool had = zero_used_;
            zero_used_ = false;
            return had;
        }
        std::size_t hole = home(key);
        for (;; hole = (hole + 1) & mask_) {
            if (slots_[hole].key == key) break;
            if (slots_[hole].key == 0) return false;
        }
        // Backward shift: pull later chain members into the hole when the
        // hole lies cyclically between their home and their slot.
        for (std::size_t j = (hole + 1) & mask_;; j = (j + 1) & mask_) {
            Slot& c = slots_[j];
            if (c.key == 0) break;
            const std::size_t h = home(c.key);
            if (((j - h) & mask_) >= ((j - hole) & mask_)) {
                slots_[hole] = c;
                hole = j;
            }
        }
        slots_[hole].key = 0;
        --size_;
        return true;
    }

    std::size_t size() const noexcept { return size_ + (zero_used_ ? 1 : 0); }
    std::size_t capacity() const noexcept { return slots_.size(); }
    std::size_t limit() const noexcept { return limit_; }

private:
    struct Slot {
        uint64_t key = 0;
        uint32_t value = 0;
    };
    std::size_t home(uint64_t key) const noexcept {
        return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> shift_);
    }

    std::vector<Slot> slots_;
    std::size_t mask_, limit_;
    int shift_ = 0;
    std::size_t size_ = 0;
    uint32_t zero_value_ = 0;
    bool zero_used_ = false;
};

}  // namespace fh
