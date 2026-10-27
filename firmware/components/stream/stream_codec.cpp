#include "stream_codec.h"

#include <cstring>

namespace ledcast::stream {
namespace {

bool lz4_decode(const uint8_t* src, size_t src_len, uint8_t* dst, size_t dst_len) {
    size_t si = 0;
    size_t di = 0;
    while (si < src_len) {
        const uint8_t token = src[si++];
        size_t literals = token >> 4;
        if (literals == 15) {
            uint8_t add = 255;
            while (add == 255) {
                if (si >= src_len || literals > dst_len - di) return false;
                add = src[si++];
                literals += add;
            }
        }
        if (literals > src_len - si || literals > dst_len - di) return false;
        // Распаковка на месте: выход идёт за входом и может его догонять.
        std::memmove(dst + di, src + si, literals);
        si += literals;
        di += literals;
        if (si == src_len) return di == dst_len;
        if (src_len - si < 2) return false;
        const size_t offset = static_cast<size_t>(src[si]) |
                              (static_cast<size_t>(src[si + 1]) << 8);
        si += 2;
        if (offset == 0 || offset > di) return false;
        size_t match = token & 0x0f;
        if (match == 15) {
            uint8_t add = 255;
            while (add == 255) {
                if (si >= src_len || match > dst_len - di) return false;
                add = src[si++];
                match += add;
            }
        }
        match += 4;
        if (match > dst_len - di) return false;
        for (size_t i = 0; i < match; ++i) dst[di + i] = dst[di + i - offset];
        di += match;
    }
    return di == dst_len;
}

}  // namespace

bool decode_frame(FrameCodec codec, const uint8_t* encoded, size_t encoded_len,
                  uint8_t* output, size_t output_len) {
    if (encoded == nullptr || output == nullptr || encoded_len == 0 || output_len == 0) {
        return false;
    }
    if (codec == FrameCodec::Raw) {
        if (encoded_len != output_len) return false;
        if (encoded != output) std::memmove(output, encoded, output_len);
        return true;
    }
    return codec == FrameCodec::Lz4 && lz4_decode(encoded, encoded_len, output, output_len);
}

bool repeat_reference(const uint8_t* encoded, size_t encoded_len, uint32_t& frame_id) {
    if (encoded == nullptr || encoded_len != kRepeatPayloadLen) return false;
    frame_id = (static_cast<uint32_t>(encoded[0]) << 24) |
               (static_cast<uint32_t>(encoded[1]) << 16) |
               (static_cast<uint32_t>(encoded[2]) << 8) | encoded[3];
    return true;
}

}  // namespace ledcast::stream
