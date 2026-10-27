#pragma once

#include <cstddef>
#include <cstdint>

namespace ledcast::stream {

inline constexpr uint16_t kDefaultPort = 4048;
inline constexpr uint8_t kFrameMagic[4] = {'L', 'C', 'F', '6'};
inline constexpr uint8_t kFrameVersion = 6;
inline constexpr size_t kFrameHeaderLen = 72;
inline constexpr uint16_t kChunkPayloadSize = 1200;
inline constexpr size_t kMaxPacketLen = kFrameHeaderLen + kChunkPayloadSize;
inline constexpr uint8_t kFrameFlagFecPresent = 0x01;

enum class PacketKind : uint8_t { Data = 0, Parity = 1 };
// Repeat: кадр совпадает с предыдущим по номеру из полезной нагрузки (4 байта,
// big-endian). Видео 30 кадров/с на выводе 60 повторяет каждый второй кадр.
enum class FrameCodec : uint8_t { Raw = 1, Lz4 = 2, Repeat = 3 };
inline constexpr uint32_t kRepeatPayloadLen = 4;

struct Header {
    PacketKind kind = PacketKind::Data;
    FrameCodec codec = FrameCodec::Raw;
    uint8_t flags = 0;
    uint16_t payload_len = 0;
    uint64_t session_id = 0;
    uint32_t stream_id = 0;
    uint32_t frame_id = 0;
    uint64_t pts_ns = 0;
    uint32_t rate_n = 0;
    uint32_t rate_d = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t decoded_bytes = 0;
    uint32_t encoded_bytes = 0;
    uint32_t frame_crc32 = 0;
    uint16_t chunk_index = 0;
    uint16_t data_chunk_count = 0;
    uint16_t chunk_payload_size = 0;
    // Порт, на котором Console отвечает на обмен метками времени (stream_clock.h).
    uint16_t clock_port = 0;
    const uint8_t* data = nullptr;
};

bool packet_parse(const uint8_t* buf, size_t len, Header& out);
uint32_t crc32(const uint8_t* data, size_t len);
bool frame_newer(uint32_t candidate, uint32_t current);

}  // namespace ledcast::stream
