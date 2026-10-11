#pragma once
#include "core/common.h"

#include <map>
#include <set>

// Best-fit sub-allocator over [0, capacity) in abstract units, with coalescing frees.
class RangeAllocator {
public:
    static constexpr u64 kFail = ~u64(0);

    void reset(u64 capacity) {
        byOffset_.clear();
        bySize_.clear();
        used_ = 0;
        capacity_ = capacity;
        if (capacity) insert(0, capacity);
    }

    u64 alloc(u64 n) {
        auto it = bySize_.lower_bound({n, 0});
        if (it == bySize_.end()) return kFail;
        auto [size, off] = *it;
        erase(off, size);
        if (size > n) insert(off + n, size - n);
        used_ += n;
        return off;
    }

    void free(u64 off, u64 n) {
        used_ -= n;
        auto next = byOffset_.lower_bound(off);
        if (next != byOffset_.end() && next->first == off + n) {
            u64 nextSize = next->second;
            erase(next->first, nextSize);
            n += nextSize;
        }
        auto prev = byOffset_.lower_bound(off);
        if (prev != byOffset_.begin()) {
            --prev;
            if (prev->first + prev->second == off) {
                u64 prevOff = prev->first, prevSize = prev->second;
                erase(prevOff, prevSize);
                off = prevOff;
                n += prevSize;
            }
        }
        insert(off, n);
    }

    u64 used() const { return used_; }
    u64 capacity() const { return capacity_; }

private:
    void insert(u64 off, u64 n) {
        byOffset_[off] = n;
        bySize_.insert({n, off});
    }
    void erase(u64 off, u64 n) {
        byOffset_.erase(off);
        bySize_.erase({n, off});
    }

    std::map<u64, u64> byOffset_;
    std::set<std::pair<u64, u64>> bySize_;
    u64 used_ = 0, capacity_ = 0;
};
