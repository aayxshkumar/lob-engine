#pragma once

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "lob/types.hpp"

namespace lob {

// OrderId -> pool slot map with open addressing and linear probing.
//
// Why not std::unordered_map: it allocates a node per insert and every lookup
// chases a bucket pointer, then a node pointer. Here a lookup hashes to one
// slot in a flat array and walks forward; with the table at most half full
// that is usually one or two adjacent 16-byte entries.
//
// - Capacity is fixed at construction (the book already bounds live orders
//   by its pool size), so the table never rehashes.
// - Empty entries are marked by value == kEmpty, so every OrderId, including
//   0, is a valid key.
// - Erase uses backward-shift deletion instead of tombstones: later entries
//   in the same probe run move back to fill the hole, so long-running books
//   don't slowly fill with dead entries.
class IdTable {
public:
    static constexpr std::uint32_t kEmpty = 0xFFFFFFFFu;

    explicit IdTable(std::size_t max_entries) {
        // At most 50% load: power of two >= 2 * max_entries.
        const std::size_t cap = std::bit_ceil(std::max<std::size_t>(2 * max_entries, 16));
        entries_.assign(cap, Entry{0, kEmpty});
        mask_ = cap - 1;
        shift_ = 64 - std::countr_zero(cap);
        max_entries_ = max_entries;
    }

    // Returns kEmpty if absent.
    std::uint32_t find(OrderId id) const {
        for (std::size_t i = home(id);; i = (i + 1) & mask_) {
            const Entry& e = entries_[i];
            if (e.value == kEmpty) return kEmpty;
            if (e.key == id) return e.value;
        }
    }

    bool contains(OrderId id) const { return find(id) != kEmpty; }

    // Caller guarantees `id` is absent.
    void insert(OrderId id, std::uint32_t value) {
        if (size_ >= max_entries_) throw std::length_error("id table full");
        std::size_t i = home(id);
        while (entries_[i].value != kEmpty) i = (i + 1) & mask_;
        entries_[i] = Entry{id, value};
        ++size_;
    }

    // Returns false if absent.
    bool erase(OrderId id) {
        std::size_t i = home(id);
        for (;; i = (i + 1) & mask_) {
            if (entries_[i].value == kEmpty) return false;
            if (entries_[i].key == id) break;
        }
        // Backward shift: walk the rest of the run and pull back any entry
        // whose home position is not strictly between the hole and itself
        // (cyclically), i.e. any entry the hole would cut off from its home.
        std::size_t hole = i;
        for (std::size_t j = (i + 1) & mask_; entries_[j].value != kEmpty; j = (j + 1) & mask_) {
            const std::size_t h = home(entries_[j].key);
            const bool reachable = hole <= j ? (hole < h && h <= j) : (hole < h || h <= j);
            if (!reachable) {
                entries_[hole] = entries_[j];
                hole = j;
            }
        }
        entries_[hole].value = kEmpty;
        --size_;
        return true;
    }

    std::size_t size() const { return size_; }

private:
    struct Entry {
        OrderId key;
        std::uint32_t value;
    };

    // Fibonacci hashing: multiply by 2^64 / phi and keep the top bits. Spreads
    // sequential ids across the table and costs one multiply.
    std::size_t home(OrderId id) const {
        return static_cast<std::size_t>((id * 0x9E3779B97F4A7C15ull) >> shift_);
    }

    std::vector<Entry> entries_;
    std::size_t mask_ = 0;
    int shift_ = 0;
    std::size_t size_ = 0;
    std::size_t max_entries_ = 0;
};

}  // namespace lob
