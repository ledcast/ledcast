#pragma once

#include <cstddef>
#include <cstdint>

namespace ledcast::stream {

// Обмен метками времени со строкой — как в NTP. Строка шлёт запрос со своим
// временем t1 на порт часов Console (он приходит в заголовке каждого кадра),
// Console отвечает временем приёма t2 и отправки t3 по своим часам, строка
// отмечает приход ответа t4. Время показа кадра (pts) — на часах Console,
// поэтому все строки одной Console меняют кадр в один момент.
inline constexpr uint8_t kClockMagic[4] = {'L', 'C', 'K', '6'};
inline constexpr uint8_t kClockVersion = 6;
inline constexpr size_t kClockPacketLen = 40;

enum class ClockKind : uint8_t { Request = 0, Reply = 1 };

struct ClockReply {
    uint64_t session_id = 0;
    int64_t t1_us = 0;
    uint64_t t2_ns = 0;
    uint64_t t3_ns = 0;
};

void clock_request_write(uint8_t* out, uint64_t session_id, int64_t t1_us);
bool clock_reply_parse(const uint8_t* buf, size_t len, ClockReply& out);

// Смещение часов Console относительно своих по последним обменам. Берётся обмен
// с самым коротким путём туда и обратно: в нём меньше всего очередей Wi‑Fi, а
// значит, и несимметрии задержки, которая одна и даёт ошибку смещения.
class SenderClock {
public:
    static constexpr uint8_t kWindow = 16;
    // Столько обменов нужно, чтобы начать показ: один обмен может прийти
    // через очередь точки доступа.
    static constexpr uint8_t kMinSamples = 4;
    // Ответ, шедший дольше, ничего не говорит о часах.
    static constexpr uint32_t kMaxRttUs = 200000;

    void reset();
    bool add(int64_t t1_us, uint64_t t2_ns, uint64_t t3_ns, int64_t t4_us);
    bool synced() const { return count_ >= kMinSamples; }
    // Время Console в момент local_us своих часов.
    int64_t sender_ns(int64_t local_us) const { return local_us * 1000 + offset_ns_; }
    uint32_t rtt_us() const { return rtt_us_; }
    uint8_t samples() const { return count_; }

private:
    struct Sample {
        int64_t offset_ns;
        uint32_t rtt_us;
    };
    Sample ring_[kWindow]{};
    uint8_t next_ = 0;
    uint8_t count_ = 0;
    int64_t offset_ns_ = 0;
    uint32_t rtt_us_ = 0;
};

}  // namespace ledcast::stream
