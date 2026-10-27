#pragma once

#include <cstdint>
#include <esp_err.h>

#include "deviceconfig.h"

namespace ledcast {

struct StreamStats {
    uint32_t packets_received;
    uint32_t header_invalid;
    uint32_t not_for_us;
    uint32_t out_of_range;
    uint32_t frames_started;
    uint32_t frames_completed;
    uint32_t frames_verified;
    uint32_t frames_shown;
    uint32_t frames_torn;
    uint32_t frames_suppressed;
    uint32_t frames_evicted;
    uint32_t frames_repeated;
    uint32_t ready_starvations;
    uint32_t frames_late;
    // Резерв был полон, а время ни одного кадра не пришло: показан раньше срока.
    uint32_t frames_early;
    uint32_t assembly_evictions;
    uint32_t assembly_overflows;
    uint32_t fec_recovered;
    uint32_t crc_or_decode_errors;
    uint32_t flip_timeouts;
    uint32_t playout_ticks;
    uint32_t playout_fps;
    uint16_t source_fps;
    uint32_t playout_jitter_us_max;
    uint32_t arrival_jitter_us_max;
    uint8_t queue_depth;
    uint8_t queue_depth_max;
    uint8_t ready_depth;
    uint8_t ready_depth_max;
    uint8_t assembly_depth;
    uint8_t assembly_depth_max;
    uint32_t raw_frames;
    uint32_t lz4_frames;
    uint32_t repeat_frames;
    uint32_t repeat_misses;
    uint32_t fps;
    uint32_t frame_age_us_avg;
    uint32_t frame_age_us_max;
    uint32_t commit_interval_us_avg;
    uint32_t commit_jitter_us_max;
    uint32_t render_us_avg;
    uint32_t render_us_max;
    uint32_t decode_us_avg;
    uint32_t decode_us_max;
    uint32_t paint_us_avg;
    uint32_t paint_us_max;
    uint32_t flip_us_avg;
    uint32_t flip_us_max;
    uint32_t display_begin_failures;
    uint32_t display_commit_failures;
    uint32_t last_offset;
    uint32_t max_offset_seen;
    int64_t last_packet_us;
    bool signal_present;
    char source_ip[16];
    uint16_t width;
    uint16_t height;
    uint32_t frame_bytes;
    // Сколько кадров строка держит до показа (stream_frame.h).
    uint8_t reservoir_frames;
    // Часы Console сверены; путь туда и обратно у лучшего обмена.
    bool clock_synced;
    uint32_t clock_rtt_us;
    uint32_t clock_samples;
};

using StreamTimeoutHandler = void (*)();

esp_err_t stream_start(const StreamConfig& cfg, StreamTimeoutHandler on_timeout);
esp_err_t stream_stop();
// Отдаёт буферы кадров куче, пока приём остановлен: пересборке дисплея нужна
// вся память, следующий stream_start выделит их заново.
esp_err_t stream_release_buffers();
// Поток принимается только с адресов, вошедших паролем строки или ключом Console,
// до until_us (esp_timer). ipv4 — в сетевом порядке байт, как sin_addr.s_addr.
void stream_allow_sender(uint32_t ipv4, int64_t until_us);
esp_err_t stream_reconfigure(const StreamConfig& cfg);
StreamStats stream_get_stats();

}  // namespace ledcast
