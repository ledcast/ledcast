#pragma once

#include <cstddef>
#include <cstdint>

#include "stream_packet.h"

namespace ledcast::stream {

inline constexpr uint16_t kMaxDataChunks = 256;
inline constexpr size_t kChunkBitmapWords = kMaxDataChunks / 32;
// Один буфер на кадр от первого пакета до показа: пакеты пишутся прямо в него,
// LZ4 распаковывается на месте, отрисовка читает оттуда же. Буферов столько,
// сколько кадров ждёт показа, плюс принимаемый и показываемый, поэтому
// задержка показа прямо задаёт память строки. Число слотов берётся по
// свободной памяти: от четырёх до двадцати.
inline constexpr uint8_t kMinFrameSlots = 4;
inline constexpr uint8_t kMaxFrameSlots = 20;
// Чётность нужна только кадру в приёме; кадров в приёме одновременно немного.
inline constexpr uint8_t kParityBuffers = 4;

// Запас за концом кадра, при котором блок LZ4, положенный в конец буфера,
// распаковывается в его начало, не затирая ещё не прочитанное
// (LZ4_DECOMPRESS_INPLACE_MARGIN; сжатый кадр короче сырого).
constexpr uint32_t frame_slot_bytes(uint32_t decoded_bytes) {
    return decoded_bytes + (decoded_bytes >> 8) + 32u;
}

// Сколько кадров строка держит до показа: все слоты, кроме того, в который
// идёт приём.
constexpr uint8_t reservoir_frames(uint8_t slots) {
    return static_cast<uint8_t>(slots - 1u);
}

// Ready — проверенный кадр ждёт времени показа, Showing — его рисует показ.
enum class AssemblyState : uint8_t { Empty, Receiving, Complete, Decoding, Ready, Showing };
enum class ChunkResult : uint8_t {
    Stored,
    Duplicate,
    Conflict,
    Complete,
    Recovered,
    Invalid,
};

struct FrameAssembly {
    AssemblyState state = AssemblyState::Empty;
    Header header{};
    uint8_t* storage = nullptr;  // буфер слота; с начала лежит готовый кадр
    uint32_t capacity = 0;
    uint8_t* encoded = nullptr;  // куда пишутся пакеты: начало, у LZ4 — конец буфера
    uint32_t chunks[kChunkBitmapWords]{};
    uint16_t received_chunks = 0;
    bool parity_present = false;
    uint8_t* parity = nullptr;   // из общего пула, только пока кадр в приёме
    int64_t first_packet_us = 0;
};

void frame_init(FrameAssembly& frame, uint8_t* storage, uint32_t capacity);
void frame_reset(FrameAssembly& frame);
// parity — буфер под пакет чётности; без него кадр собирается без FEC.
bool frame_begin(FrameAssembly& frame, const Header& header, int64_t now_us,
                 uint8_t* parity = nullptr);
bool frame_matches(const FrameAssembly& frame, const Header& header);
bool frame_has_chunk(const FrameAssembly& frame, uint16_t index);
ChunkResult frame_accept(FrameAssembly& frame, const Header& header);
bool frame_obsolete(uint32_t frame_id, bool have_committed, uint32_t highest_committed);
bool frame_due(uint64_t pts_ns, uint64_t clock_pts_ns, uint64_t period_us);
FrameAssembly* frame_next_decode(FrameAssembly* slots, size_t count);
size_t frame_complete_count(const FrameAssembly* slots, size_t count,
                            uint64_t session_id, uint32_t stream_id);
bool frame_pool_bytes(uint32_t decoded_bytes, size_t slots, size_t& pool_bytes);

}  // namespace ledcast::stream
