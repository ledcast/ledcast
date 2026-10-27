#include "stream_node.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lwip/sockets.h>

#include "stream_clock.h"
#include "stream_codec.h"
#include "stream_frame.h"
#include "stream_packet.h"
#include "stream_paint.h"
#include "stream_senders.h"
#include "display.h"
#include "frame_pacer.h"
#include "framestats.h"
#include "runtime_heap_budget.h"

namespace ledcast {
namespace {

constexpr char kTag[] = "ledcast_stream";
constexpr int kNetworkCore = 0;
constexpr int kWorkerCore = 1;
// Wi-Fi (23), esp_timer (22) and lwIP (18) live on core 0. Keep the socket
// drainer just below lwIP there; deadline-critical render outranks decode on
// the isolated worker core.
constexpr int kNetworkPriority = 17;
constexpr int kDecodePriority = 18;
constexpr int kRenderPriority = 20;
constexpr int kNetworkStack = 5120;
constexpr int kDecodeStack = 5120;
constexpr int kRenderStack = 4096;
constexpr int kRecvTimeoutMs = 100;
// Вывод пересобирается — следующая попытка показа через это время.
constexpr int64_t kDisplayRetryUs = 10000;
// Запас сверх замеренной отрисовки, с которым кадр выбирается до закрытия.
constexpr int64_t kPaintMarginUs = 500;
constexpr TickType_t kStopWaitTicks = pdMS_TO_TICKS(2000);
constexpr int64_t kFeedbackPeriodUs = 1000000;
constexpr size_t kFeedbackBytes = 72;
// Обмен метками времени: часто, пока окно оценки не набрано, дальше — чтобы
// успевать за уходом кварца.
constexpr int64_t kClockProbeFastUs = 50000;
constexpr int64_t kClockProbeUs = 250000;
// Слоты сверх минимума добавляются, только пока после них остаётся этот запас
// сверх запаса сервисов: веб, Wi‑Fi и OTA берут память пачками. Всплеск Wi‑Fi
// ограничен шестнадцатью буферами (sdkconfig.defaults): под потоком в минимуме
// остаётся 30–40 КиБ.
constexpr size_t kReservoirSpareBytes = 16u * 1024u;
// Повторы, ждущие своего времени: панель их не перерисовывает, но показанным
// кадром они считаются.
constexpr uint8_t kHeldRepeats = 8;
constexpr uint8_t kRecentFrames = 16;
// Only these allocations are still ahead of canvas allocation. Network/Web UI
// tasks, Wi-Fi pools, mutexes and the UDP socket already exist at that point.
// Counting them again in a blanket 64 KiB reserve rejected valid live resizes.
constexpr size_t kPendingTaskBytes = kNetworkStack + kDecodeStack + kRenderStack +
                                     3u * sizeof(StaticTask_t);

// Вход по паролю вносит адрес сюда из задачи веб-сервера, приёмник читает на каждом пакете.
stream::SenderList g_senders;
portMUX_TYPE g_senders_mux = portMUX_INITIALIZER_UNLOCKED;

bool sender_allowed(uint32_t ipv4, int64_t now_us) {
    taskENTER_CRITICAL(&g_senders_mux);
    const bool allowed = g_senders.allowed(ipv4, now_us);
    taskEXIT_CRITICAL(&g_senders_mux);
    return allowed;
}

struct Node {
    StreamConfig cfg{};
    StreamTimeoutHandler on_timeout = nullptr;
    SemaphoreHandle_t lock = nullptr;
    SemaphoreHandle_t network_stopped = nullptr;
    SemaphoreHandle_t decode_stopped = nullptr;
    SemaphoreHandle_t render_stopped = nullptr;
    TaskHandle_t decode_task = nullptr;
    TaskHandle_t render_task = nullptr;
    esp_timer_handle_t wake_timer = nullptr;
    int sock = -1;
    std::atomic<bool> running{false};

    std::atomic<uint32_t> active_sender{0};
    std::atomic<int64_t> active_until_us{0};
    StreamStats stats{};
    RateWindow commit_rate{1000000};
    RateWindow playout_rate{1000000};
    SampleRing frame_age;
    SampleRing arrival_interval;
    SampleRing commit_interval;
    SampleRing render_time;
    SampleRing decode_time;
    SampleRing paint_time;
    SampleRing flip_time;
    SampleRing playout_interval;
    int64_t last_arrival_us = 0;
    int64_t last_commit_us = 0;
    int64_t last_playout_us = 0;
    int64_t last_feedback_us = 0;
    bool timeout_armed = false;

    uint64_t stream_session = 0;
    uint32_t stream_id = 0;
    uint32_t session_epoch = 0;
    uint64_t retired_sessions[4]{};
    uint8_t retired_count = 0;
    uint8_t retired_next = 0;
    uint32_t highest_verified = 0;
    uint32_t highest_committed = 0;
    bool have_verified = false;
    bool have_committed = false;
    uint32_t recent_verified[kRecentFrames]{};
    uint8_t recent_next = 0;
    uint8_t recent_count = 0;
    uint32_t recent_completed[kRecentFrames]{};
    uint8_t completed_next = 0;
    uint8_t completed_count = 0;

    // Кадр живёт в одном слоте от первого пакета до показа (stream_frame.h).
    stream::FrameAssembly slots[stream::kMaxFrameSlots]{};
    uint8_t slot_count = 0;
    uint8_t* frame_pool[stream::kMaxFrameSlots]{};
    size_t frame_pool_bytes = 0;
    uint8_t parity_pool[stream::kParityBuffers][stream::kChunkPayloadSize]{};
    // Последний кадр, ушедший на панель: на него ссылаются повторы.
    uint32_t rendering_frame = 0;
    uint32_t rendering_epoch = 0;
    bool have_rendering = false;
    // Повтор не занимает слот: он отбрасывается, а панель держит кадр, который
    // он повторяет. Следующий повтор ссылается на отброшенный — запоминается,
    // какой кадр за ним стоит.
    uint32_t alias_frame = 0;
    uint32_t alias_base = 0;
    bool have_alias = false;
    uint64_t held_pts_ns = 0;       // до какого времени панель держит кадр по повторам
    uint64_t held_due[kHeldRepeats]{};
    uint8_t held_count = 0;
    uint32_t buffer_bytes = 0;

    stream::SenderClock clock;
    uint16_t clock_port = 0;        // порт часов Console из заголовка кадра
    int64_t last_probe_us = 0;
    uint64_t shown_pts_ns = 0;      // время показа у последнего выбранного кадра
    uint64_t source_period_ns = 0;  // период кадров источника из заголовка

    uint8_t rx[stream::kMaxPacketLen]{};
};

Node g_node;

uint8_t count_locked(stream::AssemblyState state) {
    uint8_t count = 0;
    for (uint8_t i = 0; i < g_node.slot_count; ++i) {
        if (g_node.slots[i].state == state) ++count;
    }
    return count;
}

uint8_t ready_count_locked() { return count_locked(stream::AssemblyState::Ready); }

uint8_t buffered_count_locked() {
    return ready_count_locked() + static_cast<uint8_t>(stream::frame_complete_count(
        g_node.slots, g_node.slot_count, g_node.stream_session, g_node.stream_id));
}

uint8_t assembly_depth_locked() {
    return count_locked(stream::AssemblyState::Receiving) +
           count_locked(stream::AssemblyState::Complete) +
           count_locked(stream::AssemblyState::Decoding);
}

void update_depth_stats_locked() {
    g_node.stats.ready_depth = ready_count_locked();
    g_node.stats.assembly_depth = assembly_depth_locked();
    g_node.stats.queue_depth = buffered_count_locked();
    g_node.stats.ready_depth_max =
        std::max(g_node.stats.ready_depth_max, g_node.stats.ready_depth);
    g_node.stats.assembly_depth_max =
        std::max(g_node.stats.assembly_depth_max, g_node.stats.assembly_depth);
    g_node.stats.queue_depth_max =
        std::max(g_node.stats.queue_depth_max, g_node.stats.queue_depth);
}

bool recently_verified_locked(uint32_t frame_id) {
    for (uint8_t i = 0; i < g_node.recent_count; ++i) {
        if (g_node.recent_verified[i] == frame_id) return true;
    }
    return false;
}

bool recently_completed_locked(uint32_t frame_id) {
    for (uint8_t i = 0; i < g_node.completed_count; ++i) {
        if (g_node.recent_completed[i] == frame_id) return true;
    }
    return false;
}

void remember_completed_locked(uint32_t frame_id) {
    if (recently_completed_locked(frame_id)) return;
    g_node.recent_completed[g_node.completed_next] = frame_id;
    g_node.completed_next = static_cast<uint8_t>((g_node.completed_next + 1) % kRecentFrames);
    if (g_node.completed_count < kRecentFrames) ++g_node.completed_count;
}

bool session_retired_locked(uint64_t session_id) {
    for (uint8_t i = 0; i < g_node.retired_count; ++i) {
        if (g_node.retired_sessions[i] == session_id) return true;
    }
    return false;
}

void retire_session_locked(uint64_t session_id) {
    if (session_id == 0 || session_retired_locked(session_id)) return;
    g_node.retired_sessions[g_node.retired_next] = session_id;
    g_node.retired_next = static_cast<uint8_t>((g_node.retired_next + 1) % 4);
    if (g_node.retired_count < 4) ++g_node.retired_count;
}

void remember_verified_locked(uint32_t frame_id) {
    g_node.recent_verified[g_node.recent_next] = frame_id;
    g_node.recent_next = static_cast<uint8_t>((g_node.recent_next + 1) % kRecentFrames);
    if (g_node.recent_count < kRecentFrames) ++g_node.recent_count;
    if (!g_node.have_verified || stream::frame_newer(frame_id, g_node.highest_verified)) {
        g_node.highest_verified = frame_id;
    }
    g_node.have_verified = true;
}

void clear_session_locked(bool retire) {
    ++g_node.session_epoch;
    if (retire) retire_session_locked(g_node.stream_session);
    // Распаковку и отрисовку их задачи закончат сами и сами освободят слот.
    for (uint8_t i = 0; i < g_node.slot_count; ++i) {
        stream::FrameAssembly& slot = g_node.slots[i];
        if (retire && slot.state == stream::AssemblyState::Receiving) ++g_node.stats.frames_torn;
        if (slot.state != stream::AssemblyState::Decoding &&
            slot.state != stream::AssemblyState::Showing) {
            stream::frame_reset(slot);
        }
    }
    g_node.stats.queue_depth = 0;
    g_node.have_rendering = false;
    g_node.have_alias = false;
    g_node.held_pts_ns = 0;
    g_node.held_count = 0;
    g_node.stream_session = 0;
    g_node.stream_id = 0;
    // Время показа нового сеанса — на часах той Console, что его начала.
    g_node.clock.reset();
    g_node.clock_port = 0;
    g_node.last_probe_us = 0;
    g_node.shown_pts_ns = 0;
    g_node.have_verified = false;
    g_node.have_committed = false;
    g_node.highest_verified = 0;
    g_node.highest_committed = 0;
    std::memset(g_node.recent_verified, 0, sizeof(g_node.recent_verified));
    g_node.recent_next = 0;
    g_node.recent_count = 0;
    std::memset(g_node.recent_completed, 0, sizeof(g_node.recent_completed));
    g_node.completed_next = 0;
    g_node.completed_count = 0;
}

void reset_reception_locked() {
    g_node.timeout_armed = false;
    g_node.stats.signal_present = false;
    clear_session_locked(false);
    std::memset(g_node.retired_sessions, 0, sizeof(g_node.retired_sessions));
    g_node.retired_count = 0;
    g_node.retired_next = 0;
    g_node.frame_age.clear();
    g_node.arrival_interval.clear();
    g_node.commit_interval.clear();
    g_node.render_time.clear();
    g_node.decode_time.clear();
    g_node.paint_time.clear();
    g_node.flip_time.clear();
    g_node.playout_interval.clear();
    g_node.last_arrival_us = 0;
    g_node.last_commit_us = 0;
    g_node.last_playout_us = 0;
    g_node.last_feedback_us = 0;
    g_node.stats.queue_depth = 0;
    g_node.source_period_ns = 0;
    g_node.shown_pts_ns = 0;
    const int64_t now = esp_timer_get_time();
    g_node.commit_rate.reset(now);
    g_node.playout_rate.reset(now);
    g_node.active_until_us.store(0, std::memory_order_release);
    g_node.active_sender.store(0, std::memory_order_release);
    g_node.stats.source_ip[0] = '\0';
}

void release_buffers() {
    for (auto& slot : g_node.slots) {
        stream::frame_init(slot, nullptr, 0);
    }
    for (auto& buffer : g_node.frame_pool) {
        free(buffer);
        buffer = nullptr;
    }
    g_node.frame_pool_bytes = 0;
    g_node.slot_count = 0;
    g_node.buffer_bytes = 0;
}

void bind_buffers(uint32_t bytes) {
    const uint32_t slot_bytes = stream::frame_slot_bytes(bytes);
    for (uint8_t i = 0; i < stream::kMaxFrameSlots; ++i) {
        const bool used = i < g_node.slot_count;
        stream::frame_init(g_node.slots[i], used ? g_node.frame_pool[i] : nullptr,
                           used ? slot_bytes : 0);
    }
    g_node.buffer_bytes = bytes;
}

esp_err_t allocate_buffers() {
    const stream::Canvas canvas = stream::paint_canvas();
    const uint32_t bytes = static_cast<uint32_t>(canvas.width) * canvas.height * 3u;
    if (bytes == 0 ||
        (bytes + stream::kChunkPayloadSize - 1u) / stream::kChunkPayloadSize > stream::kMaxDataChunks) {
        return ESP_ERR_INVALID_SIZE;
    }
    // A runtime settings update used to free this large contiguous block and
    // immediately allocate it again from the HTTP task. Temporary JSON/socket
    // allocations fragmented the heap, so an otherwise valid configuration
    // intermittently failed with ESP_ERR_NO_MEM and left the UDP port closed.
    // Keep the pool for the lifetime of a canvas and only rebind its slots.
    if (g_node.frame_pool[0] != nullptr && g_node.buffer_bytes == bytes) {
        bind_buffers(bytes);
        ESP_LOGI(kTag, "frame pool reused: %u slots, %u bytes",
                 static_cast<unsigned>(g_node.slot_count),
                 static_cast<unsigned>(g_node.frame_pool_bytes));
        return ESP_OK;
    }
    release_buffers();
    const uint32_t slot_bytes = stream::frame_slot_bytes(bytes);
    size_t pool_bytes = 0;
    if (!stream::frame_pool_bytes(bytes, stream::kMinFrameSlots, pool_bytes)) return ESP_ERR_INVALID_SIZE;
    const size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    const size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    if (!runtime_memory::frame_allocation_fits(free_bytes, largest_block, slot_bytes,
                                               pool_bytes, kPendingTaskBytes)) {
        ESP_LOGE(kTag, "frame pool rejected: need=%u free=%u largest=%u future_tasks=%u service_floor=%u",
                 static_cast<unsigned>(pool_bytes), static_cast<unsigned>(free_bytes),
                 static_cast<unsigned>(largest_block),
                 static_cast<unsigned>(kPendingTaskBytes),
                 static_cast<unsigned>(runtime_memory::kServiceHeadroom));
        return ESP_ERR_NO_MEM;
    }
    // Frames never require adjacent addresses. Allocate each independently so
    // settings changes do not require a single 55+ KB contiguous heap block.
    // Allocation remains outside the receive/decode/render hot paths.
    uint8_t count = 0;
    for (; count < stream::kMinFrameSlots; ++count) {
        g_node.frame_pool[count] = static_cast<uint8_t*>(heap_caps_malloc(slot_bytes, MALLOC_CAP_8BIT));
        if (g_node.frame_pool[count] == nullptr) {
            release_buffers();
            return ESP_ERR_NO_MEM;
        }
    }
    // Слоты сверх минимума — пока после очередного остаются запас сервисов,
    // стеки будущих задач и запас на всплески.
    while (count < stream::kMaxFrameSlots &&
           heap_caps_get_largest_free_block(MALLOC_CAP_8BIT) >= slot_bytes &&
           heap_caps_get_free_size(MALLOC_CAP_8BIT) >=
               slot_bytes + kPendingTaskBytes + runtime_memory::kServiceHeadroom + kReservoirSpareBytes) {
        g_node.frame_pool[count] = static_cast<uint8_t*>(heap_caps_malloc(slot_bytes, MALLOC_CAP_8BIT));
        if (g_node.frame_pool[count] == nullptr) break;
        ++count;
    }
    g_node.slot_count = count;
    g_node.frame_pool_bytes = static_cast<size_t>(slot_bytes) * count;
    bind_buffers(bytes);
    ESP_LOGI(kTag, "frame pool: %u slots x %u bytes = %u bytes, reservoir %u frames",
             static_cast<unsigned>(count), static_cast<unsigned>(slot_bytes),
             static_cast<unsigned>(g_node.frame_pool_bytes),
             static_cast<unsigned>(stream::reservoir_frames(count)));
    return ESP_OK;
}

int open_socket(uint16_t port) {
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return -1;
    int enable = 1;
    // Ровный поток не копит в сокете больше пары пакетов: сетевая задача
    // разбирает их по мере прихода. Очередь нужна только на пачку, а пачка от
    // отправителя, простоявшего паузу, — это уже устаревшие кадры. 32 КиБ
    // очереди при ~30 КиБ свободной кучи отдавали пачке всю память: Wi-Fi
    // терял пакеты, веб и ping переставали отвечать. 12 КиБ — полтора кадра
    // короткой строки; лишнее lwIP отбросит, не трогая кучу.
    int receive_bytes = 12 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &receive_bytes, sizeof(receive_bytes));
    timeval timeout{};
    timeout.tv_usec = kRecvTimeoutMs * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }
    return sock;
}

void begin_session_locked(const stream::Header& header) {
    clear_session_locked(true);
    g_node.stream_session = header.session_id;
    g_node.stream_id = header.stream_id;
}

// Буфер чётности, который не занят ни одним кадром в приёме.
uint8_t* free_parity_locked() {
    for (auto& buffer : g_node.parity_pool) {
        bool used = false;
        for (uint8_t i = 0; i < g_node.slot_count; ++i) used |= g_node.slots[i].parity == buffer;
        if (!used) return buffer;
    }
    return nullptr;
}

stream::FrameAssembly* select_assembly_locked(const stream::Header& header, int64_t now) {
    for (uint8_t i = 0; i < g_node.slot_count; ++i) {
        if (stream::frame_matches(g_node.slots[i], header)) return &g_node.slots[i];
    }
    for (uint8_t i = 0; i < g_node.slot_count; ++i) {
        stream::FrameAssembly& slot = g_node.slots[i];
        if (slot.state == stream::AssemblyState::Empty) {
            if (!stream::frame_begin(slot, header, now, free_parity_locked())) return nullptr;
            ++g_node.stats.frames_started;
            return &slot;
        }
    }

    stream::FrameAssembly* oldest = nullptr;
    for (uint8_t i = 0; i < g_node.slot_count; ++i) {
        stream::FrameAssembly& slot = g_node.slots[i];
        if (slot.state != stream::AssemblyState::Receiving) continue;
        if (oldest == nullptr || stream::frame_newer(oldest->header.frame_id, slot.header.frame_id)) {
            oldest = &slot;
        }
    }
    if (oldest == nullptr || !stream::frame_newer(header.frame_id, oldest->header.frame_id)) {
        return nullptr;
    }
    ++g_node.stats.frames_torn;
    ++g_node.stats.frames_evicted;
    ++g_node.stats.assembly_evictions;
    stream::frame_reset(*oldest);
    if (!stream::frame_begin(*oldest, header, now, free_parity_locked())) return nullptr;
    ++g_node.stats.frames_started;
    return oldest;
}

bool accept_packet_locked(const stream::Header& header, int64_t now) {
    const stream::Canvas canvas = stream::paint_canvas();
    if (header.width != canvas.width || header.height != canvas.height ||
        header.decoded_bytes != g_node.buffer_bytes ||
        header.encoded_bytes > g_node.buffer_bytes ||
        header.data_chunk_count > stream::kMaxDataChunks) {
        ++g_node.stats.out_of_range;
        return false;
    }
    if (g_node.stream_session == 0) {
        if (session_retired_locked(header.session_id)) {
            ++g_node.stats.not_for_us;
            return false;
        }
        begin_session_locked(header);
    } else if (header.session_id != g_node.stream_session || header.stream_id != g_node.stream_id) {
        if (session_retired_locked(header.session_id)) {
            ++g_node.stats.not_for_us;
            return false;
        }
        // Source-IP ownership was checked by network_task. A fresh session from
        // that owner is an intentional sender restart; retire the previous id
        // so delayed datagrams cannot switch the receiver back.
        begin_session_locked(header);
    }

    // A delayed parity/data duplicate can arrive after decode already reset
    // the assembly slot. It belongs to a frame whose boundary was seen, so it
    // must never allocate a fresh partial assembly with the same frame id.
    // Unlike the short duplicate caches, the committed boundary also rejects
    // datagrams delayed by more than eight frames. They must never re-enter
    // the ready queue and move the displayed image backwards.
    if (stream::frame_obsolete(header.frame_id, g_node.have_committed,
                            g_node.highest_committed) ||
        recently_completed_locked(header.frame_id) ||
        recently_verified_locked(header.frame_id)) return false;

    stream::FrameAssembly* slot = select_assembly_locked(header, now);
    if (slot == nullptr) {
        ++g_node.stats.assembly_overflows;
        return false;
    }
    const stream::ChunkResult result = stream::frame_accept(*slot, header);
    if (result == stream::ChunkResult::Conflict || result == stream::ChunkResult::Invalid) {
        ++g_node.stats.header_invalid;
        return false;
    }

    ++g_node.stats.packets_received;
    g_node.clock_port = header.clock_port;
    g_node.stats.last_packet_us = now;
    g_node.stats.last_offset = static_cast<uint32_t>(header.chunk_index) * stream::kChunkPayloadSize;
    g_node.stats.max_offset_seen = std::max(g_node.stats.max_offset_seen, g_node.stats.last_offset);
    g_node.stats.signal_present = true;
    g_node.timeout_armed = true;
    if (g_node.last_arrival_us != 0) {
        g_node.arrival_interval.push(static_cast<uint32_t>(now - g_node.last_arrival_us));
        g_node.stats.arrival_jitter_us_max = g_node.arrival_interval.spread();
    }
    g_node.last_arrival_us = now;
    const uint64_t fps = (static_cast<uint64_t>(header.rate_n) + header.rate_d / 2u) /
                         header.rate_d;
    g_node.stats.source_fps = static_cast<uint16_t>(std::min<uint64_t>(fps, UINT16_MAX));
    g_node.source_period_ns = (1000000000ULL * header.rate_d + header.rate_n / 2u) / header.rate_n;
    if (result == stream::ChunkResult::Recovered) ++g_node.stats.fec_recovered;
    const bool complete = result == stream::ChunkResult::Complete ||
                          result == stream::ChunkResult::Recovered;
    if (complete) {
        remember_completed_locked(header.frame_id);
        ++g_node.stats.frames_completed;
        if (header.codec == stream::FrameCodec::Raw) ++g_node.stats.raw_frames;
        if (header.codec == stream::FrameCodec::Lz4) ++g_node.stats.lz4_frames;
        if (header.codec == stream::FrameCodec::Repeat) ++g_node.stats.repeat_frames;
    }
    update_depth_stats_locked();
    return complete;
}

// Кадр, который повторяет Repeat, ещё лежит в слоте или стоит на панели: тогда
// повтор ничего не меняет и слот не занимает. Иначе повторять нечего — панель
// держит то, что держала.
bool repeat_held_locked(const stream::FrameAssembly& repeat, uint32_t reference) {
    if (g_node.have_alias && reference == g_node.alias_frame) reference = g_node.alias_base;
    bool held = g_node.have_rendering && g_node.rendering_epoch == g_node.session_epoch &&
                g_node.rendering_frame == reference;
    for (uint8_t i = 0; i < g_node.slot_count && !held; ++i) {
        const stream::FrameAssembly& slot = g_node.slots[i];
        held = &slot != &repeat && slot.state != stream::AssemblyState::Empty &&
               slot.state != stream::AssemblyState::Receiving &&
               slot.header.session_id == repeat.header.session_id &&
               slot.header.frame_id == reference;
    }
    if (!held) return false;
    g_node.alias_frame = repeat.header.frame_id;
    g_node.alias_base = reference;
    g_node.have_alias = true;
    g_node.held_pts_ns = std::max(g_node.held_pts_ns, repeat.header.pts_ns);
    if (g_node.held_count == kHeldRepeats) {
        std::memmove(g_node.held_due, g_node.held_due + 1, sizeof(g_node.held_due[0]) * (kHeldRepeats - 1));
        --g_node.held_count;
    }
    g_node.held_due[g_node.held_count++] = repeat.header.pts_ns;
    return true;
}

void decode_task(void*) {
    while (g_node.running.load(std::memory_order_acquire)) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        if (!g_node.running.load(std::memory_order_acquire)) break;
        while (g_node.running.load(std::memory_order_acquire)) {
            xSemaphoreTake(g_node.lock, portMAX_DELAY);
            stream::FrameAssembly* input = stream::frame_next_decode(g_node.slots, g_node.slot_count);
            if (input == nullptr) {
                xSemaphoreGive(g_node.lock);
                break;
            }
            const stream::Header header = input->header;
            if (header.codec == stream::FrameCodec::Repeat) {
                uint32_t reference = 0;
                const bool held =
                    stream::repeat_reference(input->encoded, header.encoded_bytes, reference) &&
                    repeat_held_locked(*input, reference);
                stream::frame_reset(*input);
                if (held) {
                    remember_verified_locked(header.frame_id);
                    ++g_node.stats.frames_verified;
                } else {
                    ++g_node.stats.repeat_misses;
                }
                update_depth_stats_locked();
                xSemaphoreGive(g_node.lock);
                continue;
            }
            input->state = stream::AssemblyState::Decoding;
            const uint32_t session_epoch = g_node.session_epoch;
            const int64_t first_packet_us = input->first_packet_us;
            xSemaphoreGive(g_node.lock);

            // Слот в Decoding никто, кроме этой задачи, не трогает: распаковка
            // идёт без замка, на месте, в начало того же буфера.
            const int64_t decode_started = esp_timer_get_time();
            const bool decoded = stream::decode_frame(header.codec, input->encoded,
                                                      header.encoded_bytes, input->storage,
                                                      header.decoded_bytes);
            const bool checksum_ok = decoded &&
                                     stream::crc32(input->storage, header.decoded_bytes) == header.frame_crc32;
            const int64_t now = esp_timer_get_time();

            xSemaphoreTake(g_node.lock, portMAX_DELAY);
            const bool current_session = header.session_id == g_node.stream_session &&
                                         header.stream_id == g_node.stream_id &&
                                         session_epoch == g_node.session_epoch;
            if (!checksum_ok || !current_session) {
                stream::frame_reset(*input);
                if (!checksum_ok) {
                    ++g_node.stats.crc_or_decode_errors;
                    ++g_node.stats.frames_torn;
                }
                xSemaphoreGive(g_node.lock);
                continue;
            }
            // A newer frame can commit while this decode runs without lock.
            if (stream::frame_obsolete(header.frame_id, g_node.have_committed,
                                    g_node.highest_committed)) {
                stream::frame_reset(*input);
                ++g_node.stats.frames_late;
                xSemaphoreGive(g_node.lock);
                continue;
            }
            input->state = stream::AssemblyState::Ready;
            remember_verified_locked(header.frame_id);
            ++g_node.stats.frames_verified;
            g_node.decode_time.push(static_cast<uint32_t>(now - decode_started));
            g_node.stats.decode_us_avg = g_node.decode_time.avg();
            g_node.stats.decode_us_max = g_node.decode_time.max();
            g_node.frame_age.push(static_cast<uint32_t>(now - first_packet_us));
            g_node.stats.frame_age_us_avg = g_node.frame_age.avg();
            g_node.stats.frame_age_us_max = g_node.frame_age.max();
            update_depth_stats_locked();
            xSemaphoreGive(g_node.lock);
        }
    }
    xSemaphoreGive(g_node.decode_stopped);
    vTaskDelete(nullptr);
}

void wake_timer_cb(void*) {
    if (g_node.running.load(std::memory_order_acquire) && g_node.render_task != nullptr) {
        xTaskNotifyGive(g_node.render_task);
    }
}

// Спать до deadline_us; лишнее пробуждение сон не укорачивает, остановка
// приёмника прерывает его сразу.
void render_sleep_until(int64_t deadline_us) {
    while (g_node.running.load(std::memory_order_acquire)) {
        const int64_t left = deadline_us - esp_timer_get_time();
        if (left <= 0) return;
        esp_timer_stop(g_node.wake_timer);
        esp_timer_start_once(g_node.wake_timer, static_cast<uint64_t>(left));
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    }
}

// Показ идёт по общей сетке кадров вывода (FramePacer), а не по своему
// таймеру с частотой источника. На каждой точке сетки выбирается самый новый
// кадр, чьё время уже наступило к моменту показа; нет нового — панель держит
// прежний, обмена нет. Так видео 60 кадров/с на сетке 120 Гц держит каждый
// кадр ровно два такта, без неровных повторов, а момент обмена совпадает с
// границей прохода развёртки.
void render_task(void*) {
    FramePacer pacer;
    // Сколько занимает отрисовка: кадр выбирается настолько поздно, насколько
    // она позволяет, — успевший декодироваться кадр не ждёт лишний такт.
    int64_t paint_budget_us = 2000;
    while (g_node.running.load(std::memory_order_acquire)) {
        const DisplayScanout scanout = display_scanout();
        if (scanout.pass_us == 0) {
            pacer.lost();
            render_sleep_until(esp_timer_get_time() + kDisplayRetryUs);
            continue;
        }
        const int64_t step_us = static_cast<int64_t>(scanout.pass_us) * scanout.passes_per_frame;
        const int64_t present = pacer.next_present(scanout.pass_us, scanout.passes_per_frame,
                                                   esp_timer_get_time());
        const int64_t commit_at = scanout.double_buffered
            ? FramePacer::commit_after(scanout.pass_us, present) : present;
        render_sleep_until(commit_at - paint_budget_us);
        if (!g_node.running.load(std::memory_order_acquire)) break;
        uint16_t width = 0;
        uint16_t height = 0;
        stream::FrameAssembly* showing = nullptr;
        xSemaphoreTake(g_node.lock, portMAX_DELAY);
        ++g_node.stats.playout_ticks;
        g_node.playout_rate.tick(present);
        g_node.stats.playout_fps = g_node.playout_rate.fps();
        if (g_node.last_playout_us != 0) {
            g_node.playout_interval.push(static_cast<uint32_t>(present - g_node.last_playout_us));
            g_node.stats.playout_jitter_us_max = g_node.playout_interval.spread();
        }
        g_node.last_playout_us = present;
        if (display_test_active()) {
            xSemaphoreGive(g_node.lock);
            render_sleep_until(present);
            continue;
        }

        // Кадр выбирается по часам Console: на каждой строке один и тот же кадр
        // встаёт в один и тот же момент. Пока часы не сверены, показывать нечего.
        stream::FrameAssembly* selected = nullptr;
        uint64_t due_pts = 0;
        const bool synced = g_node.clock.synced();
        g_node.stats.clock_synced = synced;
        g_node.stats.clock_rtt_us = g_node.clock.rtt_us();
        if (synced) {
            const int64_t now_ns = g_node.clock.sender_ns(present);
            due_pts = now_ns > 0 ? static_cast<uint64_t>(now_ns) : 0;
            stream::FrameAssembly* earliest = nullptr;
            bool slot_free = false;
            for (uint8_t i = 0; i < g_node.slot_count; ++i) {
                stream::FrameAssembly& slot = g_node.slots[i];
                slot_free |= slot.state == stream::AssemblyState::Empty;
                if (slot.state != stream::AssemblyState::Ready) continue;
                const uint64_t pts = slot.header.pts_ns;
                if (earliest == nullptr || pts < earliest->header.pts_ns) earliest = &slot;
                if (stream::frame_due(pts, due_pts, static_cast<uint64_t>(step_us)) &&
                    (selected == nullptr || pts > selected->header.pts_ns)) selected = &slot;
            }
            // Слотов не осталось, а время ни одного кадра не пришло: задержка
            // больше, чем строка может удержать. Лучше показать кадр раньше,
            // чем встать.
            if (selected == nullptr && earliest != nullptr && !slot_free) {
                selected = earliest;
                ++g_node.stats.frames_early;
            }
        }
        // Повтор, чьё время пришло, — показанный кадр: панель держит ту же картинку.
        while (synced && g_node.held_count != 0 && g_node.held_due[0] <= due_pts) {
            std::memmove(g_node.held_due, g_node.held_due + 1,
                         sizeof(g_node.held_due[0]) * (g_node.held_count - 1));
            --g_node.held_count;
            ++g_node.stats.frames_shown;
            g_node.commit_rate.tick(present);
            g_node.stats.fps = g_node.commit_rate.fps();
        }
        if (selected != nullptr) {
            for (uint8_t i = 0; i < g_node.slot_count; ++i) {
                stream::FrameAssembly& slot = g_node.slots[i];
                if (&slot != selected && slot.state == stream::AssemblyState::Ready &&
                    slot.header.pts_ns <= selected->header.pts_ns) {
                    stream::frame_reset(slot);
                    ++g_node.stats.frames_late;
                }
            }
            selected->state = stream::AssemblyState::Showing;
            showing = selected;
            g_node.rendering_frame = selected->header.frame_id;
            g_node.rendering_epoch = g_node.session_epoch;
            g_node.have_rendering = true;
            g_node.shown_pts_ns = selected->header.pts_ns;
            const stream::Canvas canvas = stream::paint_canvas();
            width = canvas.width;
            height = canvas.height;
        } else if (synced && g_node.shown_pts_ns != 0 && g_node.source_period_ns != 0 &&
                   due_pts > std::max(g_node.shown_pts_ns, g_node.held_pts_ns) +
                                 g_node.source_period_ns * 3u / 2u) {
            // Новый кадр источника уже должен был встать, а его нет.
            ++g_node.stats.frames_repeated;
            ++g_node.stats.ready_starvations;
        }
        update_depth_stats_locked();
        xSemaphoreGive(g_node.lock);
        if (width == 0 || height == 0) {
            render_sleep_until(present);
            continue;
        }
        if (g_node.decode_task != nullptr) xTaskNotifyGive(g_node.decode_task);

        if (!display_external_frame_begin()) {
            pacer.lost();
            xSemaphoreTake(g_node.lock, portMAX_DELAY);
            stream::frame_reset(*showing);
            ++g_node.stats.flip_timeouts;
            ++g_node.stats.display_begin_failures;
            xSemaphoreGive(g_node.lock);
            render_sleep_until(present);
            continue;
        }
        const int64_t render_started = esp_timer_get_time();
        for (uint16_t y = 0; y < height; ++y) {
            stream::paint_span(0, y, width,
                               showing->storage + static_cast<uint32_t>(y) * width * 3u);
        }
        const int64_t paint_finished = esp_timer_get_time();
        // Кадр уже в буфере вывода: слот свободен для следующего.
        xSemaphoreTake(g_node.lock, portMAX_DELAY);
        stream::frame_reset(*showing);
        xSemaphoreGive(g_node.lock);
        paint_budget_us = std::max<int64_t>(paint_finished - render_started + kPaintMarginUs,
                                            paint_budget_us - paint_budget_us / 16);
        render_sleep_until(commit_at);
        xSemaphoreTake(g_node.lock, portMAX_DELAY);
        const bool current_session = g_node.rendering_epoch == g_node.session_epoch;
        xSemaphoreGive(g_node.lock);
        const bool commit_attempted = current_session && !display_test_active();
        const int64_t flip_started = esp_timer_get_time();
        esp_err_t commit_result = ESP_ERR_INVALID_STATE;
        if (commit_attempted) commit_result = display_commit();
        const int64_t flip_finished = esp_timer_get_time();
        display_external_frame_end();
        if (commit_result == ESP_OK) {
            pacer.presented(scanout.double_buffered ? flip_finished : present);
        } else if (commit_attempted) {
            pacer.lost();
        }

        xSemaphoreTake(g_node.lock, portMAX_DELAY);
        g_node.paint_time.push(static_cast<uint32_t>(paint_finished - render_started));
        g_node.stats.paint_us_avg = g_node.paint_time.avg();
        g_node.stats.paint_us_max = g_node.paint_time.max();
        if (commit_attempted) {
            g_node.flip_time.push(static_cast<uint32_t>(flip_finished - flip_started));
            g_node.stats.flip_us_avg = g_node.flip_time.avg();
            g_node.stats.flip_us_max = g_node.flip_time.max();
        }
        g_node.render_time.push(static_cast<uint32_t>(paint_finished - render_started));
        g_node.stats.render_us_avg = g_node.render_time.avg();
        g_node.stats.render_us_max = g_node.render_time.max();
        if (commit_result == ESP_OK && g_node.rendering_epoch == g_node.session_epoch) {
            if (!g_node.have_committed ||
                stream::frame_newer(g_node.rendering_frame, g_node.highest_committed)) {
                g_node.highest_committed = g_node.rendering_frame;
            }
            g_node.have_committed = true;
            ++g_node.stats.frames_shown;
            g_node.commit_rate.tick(flip_finished);
            g_node.stats.fps = g_node.commit_rate.fps();
            if (g_node.last_commit_us != 0) {
                g_node.commit_interval.push(static_cast<uint32_t>(flip_finished - g_node.last_commit_us));
                g_node.stats.commit_interval_us_avg = g_node.commit_interval.avg();
                g_node.stats.commit_jitter_us_max = g_node.commit_interval.spread();
            }
            g_node.last_commit_us = flip_finished;
        } else if (commit_attempted) {
            ++g_node.stats.flip_timeouts;
            ++g_node.stats.display_commit_failures;
        } else {
            ++g_node.stats.frames_suppressed;
        }
        xSemaphoreGive(g_node.lock);
    }
    xSemaphoreGive(g_node.render_stopped);
    vTaskDelete(nullptr);
}

void write_be64(uint8_t* out, uint64_t value) {
    for (int i = 7; i >= 0; --i) {
        out[i] = static_cast<uint8_t>(value);
        value >>= 8;
    }
}
void write_be32(uint8_t* out, uint32_t value) {
    const uint32_t net = htonl(value);
    std::memcpy(out, &net, sizeof(net));
}
void write_be16(uint8_t* out, uint16_t value) {
    const uint16_t net = htons(value);
    std::memcpy(out, &net, sizeof(net));
}

void send_feedback(int sock, const sockaddr_in& peer, const StreamStats& stats,
                   uint64_t session, uint32_t stream, uint32_t verified,
                   uint32_t committed, uint16_t assembly_depth) {
    uint8_t packet[kFeedbackBytes]{};
    std::memcpy(packet, "LCB6", 4);
    packet[4] = 6;
    packet[5] = kFeedbackBytes;
    write_be64(packet + 8, session);
    write_be32(packet + 16, stream);
    write_be32(packet + 20, verified);
    write_be32(packet + 24, committed);
    write_be32(packet + 28, stats.packets_received);
    write_be32(packet + 32, stats.frames_torn);
    write_be32(packet + 36, stats.fec_recovered);
    write_be32(packet + 40, stats.crc_or_decode_errors);
    write_be32(packet + 44, stats.assembly_evictions);
    write_be32(packet + 48, stats.frames_late);
    write_be16(packet + 52, assembly_depth);
    write_be16(packet + 54, static_cast<uint16_t>(std::min<uint32_t>(stats.fps, UINT16_MAX)));
    write_be32(packet + 56, stats.arrival_jitter_us_max);
    write_be32(packet + 60, stats.flip_timeouts);
    write_be16(packet + 64, stats.reservoir_frames);
    packet[66] = stats.clock_synced ? 1 : 0;
    write_be32(packet + 68, stats.clock_rtt_us);
    sendto(sock, packet, sizeof(packet), MSG_DONTWAIT,
           reinterpret_cast<const sockaddr*>(&peer), sizeof(peer));
}

void network_task(void*) {
    ESP_LOGI(kTag, "Frame v6 receiver started on port %u", g_node.cfg.port);
    const int sock = g_node.sock;
    sockaddr_in feedback_peer{};
    bool have_feedback_peer = false;
    while (g_node.running.load(std::memory_order_acquire)) {
        sockaddr_in source{};
        socklen_t source_len = sizeof(source);
        const int received = recvfrom(sock, g_node.rx, sizeof(g_node.rx), 0,
                                      reinterpret_cast<sockaddr*>(&source), &source_len);
        const int64_t now = esp_timer_get_time();
        bool completed = false;
        bool timed_out = false;
        bool feedback_due = false;
        StreamStats feedback_stats{};
        uint64_t feedback_session = 0;
        uint32_t feedback_stream = 0;
        uint32_t feedback_verified = 0;
        uint32_t feedback_committed = 0;
        uint16_t feedback_depth = 0;
        int64_t hold_until = 0;
        bool probe_due = false;
        sockaddr_in probe_peer{};
        uint64_t probe_session = 0;

        stream::ClockReply clock_reply{};
        const bool is_clock_reply =
            received > 0 &&
            stream::clock_reply_parse(g_node.rx, static_cast<size_t>(received), clock_reply);
        stream::Header header{};
        const bool parsed = received > 0 && !is_clock_reply &&
                            stream::packet_parse(g_node.rx, static_cast<size_t>(received), header);
        xSemaphoreTake(g_node.lock, portMAX_DELAY);
        if (is_clock_reply) {
            // Ответ засчитывается только от Console текущего сеанса: ответ
            // прежней Console несёт время её часов.
            if (have_feedback_peer && g_node.stream_session != 0 &&
                source.sin_addr.s_addr == feedback_peer.sin_addr.s_addr &&
                clock_reply.session_id == g_node.stream_session &&
                g_node.clock.add(clock_reply.t1_us, clock_reply.t2_ns, clock_reply.t3_ns, now)) {
                ++g_node.stats.clock_samples;
            }
        } else if (received > 0 && !parsed) {
            ++g_node.stats.header_invalid;
        } else if (parsed) {
            const uint32_t owner = g_node.active_sender.load(std::memory_order_acquire);
            const int64_t owner_until = g_node.active_until_us.load(std::memory_order_acquire);
            if (!sender_allowed(source.sin_addr.s_addr, now) ||
                (owner != 0 && now < owner_until && source.sin_addr.s_addr != owner)) {
                ++g_node.stats.not_for_us;
            } else {
                const uint32_t before = g_node.stats.packets_received;
                completed = accept_packet_locked(header, now);
                if (g_node.stats.packets_received != before) {
                    g_node.active_sender.store(source.sin_addr.s_addr, std::memory_order_release);
                    hold_until = now + static_cast<int64_t>(g_node.cfg.timeout_ms) * 1000;
                    g_node.active_until_us.store(hold_until, std::memory_order_release);
                    inet_ntop(AF_INET, &source.sin_addr, g_node.stats.source_ip,
                              sizeof(g_node.stats.source_ip));
                    feedback_peer = source;
                    have_feedback_peer = true;
                }
            }
        }
        g_node.commit_rate.roll(now);
        g_node.stats.fps = g_node.commit_rate.fps();
        if (g_node.timeout_armed && g_node.stats.signal_present &&
            now - g_node.stats.last_packet_us > static_cast<int64_t>(g_node.cfg.timeout_ms) * 1000) {
            g_node.stats.signal_present = false;
            g_node.stats.fps = 0;
            g_node.timeout_armed = false;
            g_node.active_sender.store(0, std::memory_order_release);
            g_node.active_until_us.store(0, std::memory_order_release);
            g_node.stats.source_ip[0] = '\0';
            clear_session_locked(true);
            // After the lease expires the same sender session may legitimately
            // resume; retired ids only protect an active handover.
            std::memset(g_node.retired_sessions, 0, sizeof(g_node.retired_sessions));
            g_node.retired_count = 0;
            g_node.retired_next = 0;
            g_node.commit_rate.reset(now);
            g_node.last_commit_us = 0;
            have_feedback_peer = false;
            timed_out = true;
        }
        if (have_feedback_peer && now - g_node.last_feedback_us >= kFeedbackPeriodUs) {
            g_node.last_feedback_us = now;
            feedback_stats = g_node.stats;
            feedback_session = g_node.stream_session;
            feedback_stream = g_node.stream_id;
            feedback_verified = g_node.have_verified ? g_node.highest_verified : 0;
            feedback_committed = g_node.have_committed ? g_node.highest_committed : 0;
            feedback_depth = assembly_depth_locked();
            feedback_due = true;
        }
        if (have_feedback_peer && g_node.stream_session != 0 && g_node.clock_port != 0) {
            const int64_t probe_period = g_node.clock.samples() < stream::SenderClock::kWindow
                ? kClockProbeFastUs : kClockProbeUs;
            if (now - g_node.last_probe_us >= probe_period) {
                g_node.last_probe_us = now;
                probe_due = true;
                probe_peer = feedback_peer;
                probe_peer.sin_port = htons(g_node.clock_port);
                probe_session = g_node.stream_session;
            }
        }
        xSemaphoreGive(g_node.lock);

        if (probe_due) {
            uint8_t probe[stream::kClockPacketLen];
            stream::clock_request_write(probe, probe_session, esp_timer_get_time());
            sendto(sock, probe, sizeof(probe), MSG_DONTWAIT,
                   reinterpret_cast<const sockaddr*>(&probe_peer), sizeof(probe_peer));
        }

        if (completed && g_node.decode_task != nullptr) xTaskNotifyGive(g_node.decode_task);
        if (feedback_due) {
            send_feedback(sock, feedback_peer, feedback_stats, feedback_session, feedback_stream,
                          feedback_verified, feedback_committed, feedback_depth);
        }
        if (hold_until != 0) display_external_hold(hold_until);
        if (timed_out) {
            display_external_drop();
            if (g_node.on_timeout != nullptr) g_node.on_timeout();
        }
    }
    close(sock);
    g_node.sock = -1;
    xSemaphoreGive(g_node.network_stopped);
    vTaskDelete(nullptr);
}

esp_err_t validate(const StreamConfig& cfg) {
    return cfg.port != 0 && cfg.timeout_ms == 2000 ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t ensure_primitives() {
    if (g_node.lock == nullptr) g_node.lock = xSemaphoreCreateMutex();
    if (g_node.network_stopped == nullptr) g_node.network_stopped = xSemaphoreCreateBinary();
    if (g_node.decode_stopped == nullptr) g_node.decode_stopped = xSemaphoreCreateBinary();
    if (g_node.render_stopped == nullptr) g_node.render_stopped = xSemaphoreCreateBinary();
    if (g_node.wake_timer == nullptr) {
        esp_timer_create_args_t args{};
        args.callback = wake_timer_cb;
        args.name = "ledcast_render";
        args.skip_unhandled_events = true;
        if (esp_timer_create(&args, &g_node.wake_timer) != ESP_OK) g_node.wake_timer = nullptr;
    }
    return g_node.lock != nullptr && g_node.network_stopped != nullptr &&
                   g_node.decode_stopped != nullptr && g_node.render_stopped != nullptr &&
                   g_node.wake_timer != nullptr
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

esp_err_t start_tasks(const StreamConfig& cfg, StreamTimeoutHandler on_timeout) {
    esp_err_t err = ensure_primitives();
    if (err != ESP_OK) return err;
    const int sock = open_socket(cfg.port);
    if (sock < 0) {
        return ESP_FAIL;
    }
    // Account for the real socket allocation instead of estimating it again
    // in the frame reserve. Failed admission owns and closes this socket.
    err = allocate_buffers();
    if (err != ESP_OK) { close(sock); return err; }
    xSemaphoreTake(g_node.network_stopped, 0);
    xSemaphoreTake(g_node.decode_stopped, 0);
    xSemaphoreTake(g_node.render_stopped, 0);
    g_node.cfg = cfg;
    g_node.on_timeout = on_timeout;
    g_node.sock = sock;
    g_node.stats = StreamStats{};
    g_node.stats.width = stream::paint_canvas().width;
    g_node.stats.height = stream::paint_canvas().height;
    g_node.stats.frame_bytes = g_node.buffer_bytes;
    g_node.stats.reservoir_frames = stream::reservoir_frames(g_node.slot_count);
    reset_reception_locked();
    g_node.running.store(true, std::memory_order_release);
    g_node.render_task = nullptr;
    g_node.decode_task = nullptr;

    if (xTaskCreatePinnedToCore(render_task, "stream_render", kRenderStack, nullptr,
                                kRenderPriority, &g_node.render_task, kWorkerCore) != pdPASS) {
        g_node.running.store(false, std::memory_order_release);
        close(sock);
        g_node.sock = -1;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(decode_task, "stream_decode", kDecodeStack, nullptr,
                                kDecodePriority, &g_node.decode_task, kWorkerCore) != pdPASS) {
        g_node.running.store(false, std::memory_order_release);
        xTaskNotifyGive(g_node.render_task);
        xSemaphoreTake(g_node.render_stopped, kStopWaitTicks);
        g_node.render_task = nullptr;
        close(sock);
        g_node.sock = -1;
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreatePinnedToCore(network_task, "stream_rx", kNetworkStack, nullptr,
                                kNetworkPriority, nullptr, kNetworkCore) != pdPASS) {
        g_node.running.store(false, std::memory_order_release);
        xTaskNotifyGive(g_node.render_task);
        xTaskNotifyGive(g_node.decode_task);
        xSemaphoreTake(g_node.decode_stopped, kStopWaitTicks);
        xSemaphoreTake(g_node.render_stopped, kStopWaitTicks);
        g_node.decode_task = nullptr;
        g_node.render_task = nullptr;
        close(sock);
        g_node.sock = -1;
        return ESP_ERR_NO_MEM;
    }
    const size_t remaining = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    ESP_LOGI(kTag, "stream memory: frames=%u tasks=%u remaining=%u floor=%u",
             static_cast<unsigned>(g_node.frame_pool_bytes), static_cast<unsigned>(kPendingTaskBytes),
             static_cast<unsigned>(remaining), static_cast<unsigned>(runtime_memory::kServiceHeadroom));
    // This catches allocator overhead and allocations on the other core made
    // since preflight. Nothing is committed to configuration on failure.
    if (remaining < runtime_memory::kServiceHeadroom) {
        const esp_err_t stopped = stream_stop();
        if (stopped != ESP_OK) return stopped;
        release_buffers();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

}  // namespace

esp_err_t stream_start(const StreamConfig& cfg, StreamTimeoutHandler on_timeout) {
    if (g_node.running.load(std::memory_order_acquire)) return ESP_ERR_INVALID_STATE;
    const esp_err_t err = validate(cfg);
    if (err != ESP_OK) return err;
    g_node.on_timeout = on_timeout;
    if (!cfg.enabled) {
        g_node.cfg = cfg;
        return ESP_OK;
    }
    return start_tasks(cfg, on_timeout);
}

void stream_allow_sender(uint32_t ipv4, int64_t until_us) {
    taskENTER_CRITICAL(&g_senders_mux);
    g_senders.allow(ipv4, until_us);
    taskEXIT_CRITICAL(&g_senders_mux);
}

esp_err_t stream_release_buffers() {
    if (g_node.running.load(std::memory_order_acquire)) return ESP_ERR_INVALID_STATE;
    release_buffers();
    return ESP_OK;
}

esp_err_t stream_stop() {
    if (!g_node.running.load(std::memory_order_acquire)) return ESP_OK;
    g_node.running.store(false, std::memory_order_release);
    if (g_node.wake_timer != nullptr) esp_timer_stop(g_node.wake_timer);
    if (g_node.render_task != nullptr) xTaskNotifyGive(g_node.render_task);
    if (g_node.decode_task != nullptr) xTaskNotifyGive(g_node.decode_task);
    const bool network_done = xSemaphoreTake(g_node.network_stopped, kStopWaitTicks) == pdTRUE;
    const bool decode_done = xSemaphoreTake(g_node.decode_stopped, kStopWaitTicks) == pdTRUE;
    const bool render_done = xSemaphoreTake(g_node.render_stopped, kStopWaitTicks) == pdTRUE;
    if (!network_done || !decode_done || !render_done) return ESP_ERR_TIMEOUT;
    g_node.decode_task = nullptr;
    g_node.render_task = nullptr;
    display_external_drop();
    xSemaphoreTake(g_node.lock, portMAX_DELAY);
    reset_reception_locked();
    g_node.stats.fps = 0;
    xSemaphoreGive(g_node.lock);
    // Retain the contiguous frame pool. Reallocating it synchronously from a
    // Web UI request is both unnecessary and unreliable on a fragmented heap;
    // allocate_buffers() will reuse it when the canvas size is unchanged.
    ESP_LOGI(kTag, "Frame v6 receiver stopped");
    return ESP_OK;
}

esp_err_t stream_reconfigure(const StreamConfig& cfg) {
    const esp_err_t err = validate(cfg);
    if (err != ESP_OK) return err;
    if (!cfg.enabled) {
        if (g_node.running.load(std::memory_order_acquire)) return stream_stop();
        g_node.cfg = cfg;
        return ESP_OK;
    }
    if (!g_node.running.load(std::memory_order_acquire)) return start_tasks(cfg, g_node.on_timeout);
    if (std::memcmp(&cfg, &g_node.cfg, sizeof(cfg)) == 0) return ESP_OK;
    const StreamTimeoutHandler handler = g_node.on_timeout;
    const esp_err_t stopped = stream_stop();
    return stopped == ESP_OK ? start_tasks(cfg, handler) : stopped;
}

StreamStats stream_get_stats() {
    if (g_node.lock == nullptr) return g_node.stats;
    xSemaphoreTake(g_node.lock, portMAX_DELAY);
    const StreamStats copy = g_node.stats;
    xSemaphoreGive(g_node.lock);
    return copy;
}

}  // namespace ledcast
