#include "stream_clock.h"

#include <cstring>

namespace ledcast::stream {
namespace {

void put64(uint8_t* out, uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        out[i] = static_cast<uint8_t>(value);
        value >>= 8;
    }
}

uint64_t get64(const uint8_t* in) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value = (value << 8) | in[i];
    return value;
}

}  // namespace

void clock_request_write(uint8_t* out, uint64_t session_id, int64_t t1_us) {
    std::memset(out, 0, kClockPacketLen);
    std::memcpy(out, kClockMagic, sizeof(kClockMagic));
    out[4] = kClockVersion;
    out[5] = static_cast<uint8_t>(ClockKind::Request);
    put64(out + 8, session_id);
    put64(out + 16, static_cast<uint64_t>(t1_us));
}

bool clock_reply_parse(const uint8_t* buf, size_t len, ClockReply& out) {
    if (buf == nullptr || len != kClockPacketLen ||
        std::memcmp(buf, kClockMagic, sizeof(kClockMagic)) != 0 || buf[4] != kClockVersion ||
        buf[5] != static_cast<uint8_t>(ClockKind::Reply)) {
        return false;
    }
    out.session_id = get64(buf + 8);
    out.t1_us = static_cast<int64_t>(get64(buf + 16));
    out.t2_ns = get64(buf + 24);
    out.t3_ns = get64(buf + 32);
    return true;
}

void SenderClock::reset() {
    *this = SenderClock{};
}

bool SenderClock::add(int64_t t1_us, uint64_t t2_ns, uint64_t t3_ns, int64_t t4_us) {
    if (t4_us < t1_us || t3_ns < t2_ns) return false;
    const int64_t held_us = static_cast<int64_t>((t3_ns - t2_ns) / 1000u);
    const int64_t rtt_us = (t4_us - t1_us) - held_us;
    if (rtt_us < 0 || rtt_us > static_cast<int64_t>(kMaxRttUs)) return false;
    const int64_t forward_ns = static_cast<int64_t>(t2_ns) - t1_us * 1000;
    const int64_t backward_ns = static_cast<int64_t>(t3_ns) - t4_us * 1000;
    ring_[next_] = Sample{forward_ns / 2 + backward_ns / 2, static_cast<uint32_t>(rtt_us)};
    next_ = static_cast<uint8_t>((next_ + 1) % kWindow);
    if (count_ < kWindow) ++count_;

    const Sample* best = &ring_[0];
    for (uint8_t i = 1; i < count_; ++i) {
        if (ring_[i].rtt_us < best->rtt_us) best = &ring_[i];
    }
    offset_ns_ = best->offset_ns;
    rtt_us_ = best->rtt_us;
    return true;
}

}  // namespace ledcast::stream
