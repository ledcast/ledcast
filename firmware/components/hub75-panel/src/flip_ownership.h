#pragma once

#include <cstdint>

namespace hub75_flip {

// Software ownership for the two DMA frame buffers.
//
// A flip first removes the writable buffer from the CPU and makes it pending.
// Only an observed DMA transition releases the old visible buffer back to the
// CPU. A timeout deliberately leaves both buffers unavailable: guessing which
// one DMA owns is precisely what causes writes into the live scanout buffer.
class Ownership {
 public:
    explicit constexpr Ownership(uint8_t writable = 0) : writable_(writable & 1u) {}

    constexpr bool writable() const { return !pending_; }
    constexpr bool pending() const { return pending_; }
    constexpr uint8_t writable_id() const { return writable_; }
    constexpr uint8_t pending_id() const { return writable_; }
    constexpr uint8_t visible_id() const { return static_cast<uint8_t>(writable_ ^ 1u); }

    constexpr uint8_t begin_flip() {
        pending_ = true;
        return pending_id();
    }

    constexpr void timeout() { pending_ = true; }

    constexpr void confirm() {
        if (!pending_) return;
        writable_ ^= 1u;
        pending_ = false;
    }

 private:
    uint8_t writable_ = 0;
    bool pending_ = false;
};

}  // namespace hub75_flip
