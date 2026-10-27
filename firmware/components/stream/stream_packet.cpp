#include "stream_packet.h"

#include <cstring>

namespace ledcast::stream {
namespace {
uint16_t read_be16(const uint8_t* p) {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) | p[1]);
}
uint32_t read_be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
uint64_t read_be64(const uint8_t* p) {
    return (static_cast<uint64_t>(read_be32(p)) << 32) | read_be32(p + 4);
}
}  // namespace

uint32_t crc32(const uint8_t* data, size_t len) {
    // Reflected IEEE CRC-32, four bits per lookup. This removes six of the
    // eight dependency-heavy bit iterations per byte without spending the
    // 1 KiB flash table of a byte-at-a-time implementation.
    static constexpr uint32_t kNibble[16] = {
        0x00000000u, 0x1db71064u, 0x3b6e20c8u, 0x26d930acu,
        0x76dc4190u, 0x6b6b51f4u, 0x4db26158u, 0x5005713cu,
        0xedb88320u, 0xf00f9344u, 0xd6d6a3e8u, 0xcb61b38cu,
        0x9b64c2b0u, 0x86d3d2d4u, 0xa00ae278u, 0xbdbdf21cu,
    };
    uint32_t crc = ~0u;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        crc = (crc >> 4) ^ kNibble[crc & 0x0fu];
        crc = (crc >> 4) ^ kNibble[crc & 0x0fu];
    }
    return ~crc;
}

bool frame_newer(uint32_t candidate, uint32_t current) {
    return static_cast<int32_t>(candidate - current) > 0;
}

bool packet_parse(const uint8_t* buf, size_t len, Header& out) {
    if (buf == nullptr || len < kFrameHeaderLen ||
        std::memcmp(buf, kFrameMagic, sizeof(kFrameMagic)) != 0 ||
        buf[4] != kFrameVersion || buf[5] > static_cast<uint8_t>(PacketKind::Parity) ||
        buf[6] < static_cast<uint8_t>(FrameCodec::Raw) ||
        buf[6] > static_cast<uint8_t>(FrameCodec::Repeat) ||
        (buf[7] & ~kFrameFlagFecPresent) != 0 ||
        (buf[7] & kFrameFlagFecPresent) == 0 ||
        read_be16(buf + 8) != kFrameHeaderLen ||
        read_be16(buf + 66) == 0 || read_be32(buf + 68) != crc32(buf, 68)) {
        return false;
    }

    Header parsed{};
    parsed.kind = static_cast<PacketKind>(buf[5]);
    parsed.codec = static_cast<FrameCodec>(buf[6]);
    parsed.flags = buf[7];
    parsed.payload_len = read_be16(buf + 10);
    parsed.session_id = read_be64(buf + 12);
    parsed.stream_id = read_be32(buf + 20);
    parsed.frame_id = read_be32(buf + 24);
    parsed.pts_ns = read_be64(buf + 28);
    parsed.rate_n = read_be32(buf + 36);
    parsed.rate_d = read_be32(buf + 40);
    parsed.width = read_be16(buf + 44);
    parsed.height = read_be16(buf + 46);
    parsed.decoded_bytes = read_be32(buf + 48);
    parsed.encoded_bytes = read_be32(buf + 52);
    parsed.frame_crc32 = read_be32(buf + 56);
    parsed.chunk_index = read_be16(buf + 60);
    parsed.data_chunk_count = read_be16(buf + 62);
    parsed.chunk_payload_size = read_be16(buf + 64);
    parsed.clock_port = read_be16(buf + 66);

    const uint64_t expected_decoded =
        static_cast<uint64_t>(parsed.width) * parsed.height * 3u;
    if (parsed.session_id == 0 || parsed.stream_id == 0 || parsed.width == 0 ||
        parsed.height == 0 || parsed.rate_n == 0 || parsed.rate_d == 0 ||
        expected_decoded != parsed.decoded_bytes || parsed.encoded_bytes == 0 ||
        parsed.chunk_payload_size != kChunkPayloadSize || parsed.data_chunk_count == 0 ||
        len != kFrameHeaderLen + parsed.payload_len) {
        return false;
    }
    if (parsed.codec == FrameCodec::Raw && parsed.encoded_bytes != parsed.decoded_bytes) {
        return false;
    }
    if (parsed.codec == FrameCodec::Repeat && parsed.encoded_bytes != kRepeatPayloadLen) {
        return false;
    }
    const uint32_t expected_chunks =
        (parsed.encoded_bytes + kChunkPayloadSize - 1u) / kChunkPayloadSize;
    if (expected_chunks != parsed.data_chunk_count) return false;

    if (parsed.kind == PacketKind::Data) {
        if (parsed.chunk_index >= parsed.data_chunk_count) return false;
        const uint32_t offset = static_cast<uint32_t>(parsed.chunk_index) * kChunkPayloadSize;
        const uint16_t expected_payload = static_cast<uint16_t>(
            parsed.encoded_bytes - offset < kChunkPayloadSize
                ? parsed.encoded_bytes - offset
                : kChunkPayloadSize);
        if (parsed.payload_len != expected_payload) return false;
    } else if (parsed.chunk_index != parsed.data_chunk_count ||
               // Чётность длиной в самый длинный фрагмент: короткому кадру
               // незачем тащить 1200 байт нулей.
               parsed.payload_len != (parsed.encoded_bytes < kChunkPayloadSize
                                          ? parsed.encoded_bytes
                                          : kChunkPayloadSize)) {
        return false;
    }

    parsed.data = buf + kFrameHeaderLen;
    out = parsed;
    return true;
}

}  // namespace ledcast::stream
