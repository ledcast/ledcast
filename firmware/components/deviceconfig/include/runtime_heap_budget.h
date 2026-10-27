#pragma once
#include <cstddef>

namespace ledcast::runtime_memory {
// The same floor used by HTTP admission: allocating a video canvas must not
// permanently make the control interface reject every subsequent request.
inline constexpr size_t kServiceHeadroom = 24u * 1024u;

constexpr bool frame_allocation_fits(size_t free_bytes, size_t largest_block,
                                      size_t frame_bytes, size_t pool_bytes,
                                      size_t pending_task_bytes) {
    if (!frame_bytes || !pool_bytes || largest_block < frame_bytes || free_bytes < pool_bytes)
        return false;
    const size_t after_frames = free_bytes - pool_bytes;
    return after_frames >= pending_task_bytes &&
           after_frames - pending_task_bytes >= kServiceHeadroom;
}
} // namespace ledcast::runtime_memory
