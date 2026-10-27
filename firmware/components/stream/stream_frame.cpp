#include "stream_frame.h"

#include <algorithm>
#include <cstring>

namespace ledcast::stream {
namespace {

bool same_metadata(const Header& a, const Header& b) {
    return a.codec == b.codec && a.flags == b.flags && a.session_id == b.session_id &&
           a.stream_id == b.stream_id && a.frame_id == b.frame_id &&
           a.pts_ns == b.pts_ns && a.rate_n == b.rate_n && a.rate_d == b.rate_d &&
           a.width == b.width && a.height == b.height &&
           a.decoded_bytes == b.decoded_bytes && a.encoded_bytes == b.encoded_bytes &&
           a.frame_crc32 == b.frame_crc32 &&
           a.data_chunk_count == b.data_chunk_count &&
           a.chunk_payload_size == b.chunk_payload_size;
}

uint16_t chunk_length(const Header& header, uint16_t index) {
    const uint32_t offset = static_cast<uint32_t>(index) * kChunkPayloadSize;
    const uint32_t remaining = header.encoded_bytes - offset;
    return static_cast<uint16_t>(remaining < kChunkPayloadSize ? remaining
                                                               : kChunkPayloadSize);
}

void mark_chunk(FrameAssembly& frame, uint16_t index) {
    frame.chunks[index / 32u] |= 1u << (index % 32u);
    ++frame.received_chunks;
}

bool recover_one(FrameAssembly& frame) {
    if (!frame.parity_present || frame.parity == nullptr || frame.received_chunks + 1 != frame.header.data_chunk_count) {
        return false;
    }
    uint16_t missing = 0;
    while (missing < frame.header.data_chunk_count && frame_has_chunk(frame, missing)) ++missing;
    if (missing == frame.header.data_chunk_count) return false;

    uint8_t recovered[kChunkPayloadSize];
    std::memcpy(recovered, frame.parity, sizeof(recovered));
    for (uint16_t index = 0; index < frame.header.data_chunk_count; ++index) {
        if (index == missing) continue;
        const uint32_t offset = static_cast<uint32_t>(index) * kChunkPayloadSize;
        const uint16_t length = chunk_length(frame.header, index);
        for (uint16_t i = 0; i < length; ++i) recovered[i] ^= frame.encoded[offset + i];
    }
    const uint32_t target = static_cast<uint32_t>(missing) * kChunkPayloadSize;
    std::memcpy(frame.encoded + target, recovered, chunk_length(frame.header, missing));
    mark_chunk(frame, missing);
    return true;
}

}  // namespace

bool frame_obsolete(uint32_t frame_id, bool have_committed, uint32_t highest_committed) {
    return have_committed && !frame_newer(frame_id, highest_committed);
}

bool frame_due(uint64_t pts_ns, uint64_t clock_pts_ns, uint64_t period_us) {
    if (pts_ns <= clock_pts_ns) return true;
    // Center the decision between nominal frame deadlines rather than on a
    // boundary where microsecond timer jitter causes repeat/drop alternation.
    // Very low source rates must not permit arbitrarily early presentation.
    const uint64_t margin_ns = std::min<uint64_t>(period_us / 2, 10000) * 1000;
    return pts_ns - clock_pts_ns <= margin_ns;
}

FrameAssembly* frame_next_decode(FrameAssembly* slots, size_t count) {
    FrameAssembly* next = nullptr;
    for (size_t i = 0; i < count; ++i) {
        auto& slot = slots[i];
        if (slot.state == AssemblyState::Complete &&
            (next == nullptr || slot.header.pts_ns < next->header.pts_ns)) next = &slot;
    }
    return next;
}

size_t frame_complete_count(const FrameAssembly* slots, size_t count,
                            uint64_t session_id, uint32_t stream_id) {
    size_t buffered = 0;
    for (size_t i = 0; i < count; ++i) {
        const auto& slot = slots[i];
        if ((slot.state == AssemblyState::Complete || slot.state == AssemblyState::Decoding) &&
            slot.header.session_id == session_id && slot.header.stream_id == stream_id) ++buffered;
    }
    return buffered;
}

bool frame_pool_bytes(uint32_t decoded_bytes, size_t slots, size_t& pool_bytes) {
    if (decoded_bytes == 0 || slots == 0) return false;
    const size_t slot = frame_slot_bytes(decoded_bytes);
    pool_bytes = slot * slots;
    return pool_bytes / slots == slot;
}

void frame_init(FrameAssembly& frame, uint8_t* storage, uint32_t capacity) {
    frame = FrameAssembly{};
    frame.storage = storage;
    frame.capacity = capacity;
}

void frame_reset(FrameAssembly& frame) {
    uint8_t* storage = frame.storage;
    const uint32_t capacity = frame.capacity;
    frame_init(frame, storage, capacity);
}

bool frame_begin(FrameAssembly& frame, const Header& header, int64_t now_us, uint8_t* parity) {
    if (frame.state != AssemblyState::Empty || frame.storage == nullptr ||
        header.decoded_bytes > frame.capacity || header.encoded_bytes > frame.capacity ||
        header.data_chunk_count > kMaxDataChunks) {
        return false;
    }
    // Сжатый кадр ложится в конец буфера и распаковывается в его начало.
    const bool in_place = header.codec == FrameCodec::Lz4;
    if (in_place && frame.capacity - header.decoded_bytes <
                        frame_slot_bytes(header.decoded_bytes) - header.decoded_bytes) {
        return false;
    }
    frame.header = header;
    frame.header.data = nullptr;
    frame.encoded = frame.storage + (in_place ? frame.capacity - header.encoded_bytes : 0u);
    frame.parity = parity;
    frame.state = AssemblyState::Receiving;
    frame.first_packet_us = now_us;
    return true;
}

bool frame_matches(const FrameAssembly& frame, const Header& header) {
    return frame.state != AssemblyState::Empty && same_metadata(frame.header, header);
}

bool frame_has_chunk(const FrameAssembly& frame, uint16_t index) {
    return index < frame.header.data_chunk_count &&
           (frame.chunks[index / 32u] & (1u << (index % 32u))) != 0;
}

ChunkResult frame_accept(FrameAssembly& frame, const Header& header) {
    if (!frame_matches(frame, header)) {
        return ChunkResult::Invalid;
    }
    if (frame.state != AssemblyState::Receiving) {
        // Собранный кадр уже распаковывается или распакован на месте пакетов:
        // сверять опоздавший дубль не с чем.
        if (frame.state != AssemblyState::Complete || header.kind == PacketKind::Parity) {
            return ChunkResult::Duplicate;
        }
        const uint32_t offset = static_cast<uint32_t>(header.chunk_index) * kChunkPayloadSize;
        return frame_has_chunk(frame, header.chunk_index) &&
                       std::memcmp(frame.encoded + offset, header.data, header.payload_len) == 0
                   ? ChunkResult::Duplicate
                   : ChunkResult::Conflict;
    }
    if (header.kind == PacketKind::Parity) {
        if (frame.parity == nullptr) return ChunkResult::Duplicate;  // FEC без буфера
        if (frame.parity_present) {
            return std::memcmp(frame.parity, header.data, header.payload_len) == 0
                       ? ChunkResult::Duplicate
                       : ChunkResult::Conflict;
        }
        std::memcpy(frame.parity, header.data, header.payload_len);
        frame.parity_present = true;
    } else {
        const uint32_t offset = static_cast<uint32_t>(header.chunk_index) * kChunkPayloadSize;
        if (frame_has_chunk(frame, header.chunk_index)) {
            return std::memcmp(frame.encoded + offset, header.data, header.payload_len) == 0
                       ? ChunkResult::Duplicate
                       : ChunkResult::Conflict;
        }
        std::memcpy(frame.encoded + offset, header.data, header.payload_len);
        mark_chunk(frame, header.chunk_index);
    }

    bool recovered = false;
    if (frame.received_chunks != frame.header.data_chunk_count) recovered = recover_one(frame);
    if (frame.received_chunks == frame.header.data_chunk_count) {
        frame.state = AssemblyState::Complete;
        frame.parity = nullptr;  // буфер чётности возвращается в пул
        return recovered ? ChunkResult::Recovered : ChunkResult::Complete;
    }
    return ChunkResult::Stored;
}

}  // namespace ledcast::stream
