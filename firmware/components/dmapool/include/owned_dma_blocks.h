#pragma once
#include <cstddef>
#include <limits>

namespace ledcast {
// Two frames of 32 address rows, two descriptor rings and the shared scan pad.
// Contiguity is required within each payload, not between rows or between
// frame buffers.
class OwnedDmaBlocks {
public:
    static constexpr size_t kMaxBlocks = 67;
    explicit constexpr OwnedDmaBlocks(size_t limit) : limit_(limit) {}
    using Allocate = void* (*)(size_t);
    using Release = void (*)(void*);
    void* take(size_t bytes, Allocate allocate) {
        if (!bytes || bytes > std::numeric_limits<size_t>::max() - 3u || count_ == kMaxBlocks)
            return nullptr;
        const size_t aligned = (bytes + 3u) & ~size_t(3u);
        if (aligned > limit_ - used_) return nullptr;
        void* block = allocate(aligned);
        if (!block) return nullptr;
        blocks_[count_++] = block;
        used_ += aligned;
        return block;
    }
    // Only after DMA and every other user have relinquished these pointers.
    void clear(Release release) {
        while (count_) {
            void*& block = blocks_[--count_];
            release(block);
            block = nullptr;
        }
        used_ = 0;
    }
    size_t used() const { return used_; }
    size_t limit() const { return limit_; }
private:
    void* blocks_[kMaxBlocks]{};
    size_t count_ = 0;
    size_t used_ = 0;
    size_t limit_;
};
} // namespace ledcast
