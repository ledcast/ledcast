#pragma once

#include <cstddef>
#include <cstdint>

#include "stream_packet.h"

namespace ledcast::stream {

// Decodes one independent frame and requires the exact declared output size.
bool decode_frame(FrameCodec codec, const uint8_t* encoded, size_t encoded_len,
                  uint8_t* output, size_t output_len);

// Номер кадра, который повторяет Repeat. Сам кадр копирует приёмник: только он
// знает, в каком буфере лежит предыдущий.
bool repeat_reference(const uint8_t* encoded, size_t encoded_len, uint32_t& frame_id);

}  // namespace ledcast::stream
