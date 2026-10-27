#include <cstdio>

#include <esp_mac.h>
#include <esp_timer.h>

#include "stream_node.h"
#include "deviceconfig.h"
#include "display.h"
#include "health.h"
#include "netconfig.h"
#include "webui_internal.h"
#include "webui.h"

namespace ledcast::webui {
namespace {
const char* net_state_name(NetState state) {
    switch (state) { case NetState::Down: return "down"; case NetState::ApOnly: return "ap"; case NetState::StaConnecting: return "connecting"; case NetState::StaConnected: return "sta"; case NetState::ApSta: return "ap_sta"; }
    return "down";
}
}  // namespace

void device_id(char out[18]) { uint8_t mac[6]{}; esp_read_mac(mac, ESP_MAC_WIFI_STA); snprintf(out, 18, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]); }

void write_status(JsonSink& json) {
    const auto snapshot = config_get(); const auto& cfg = *snapshot; const HealthStats health = health_get_stats(); const NetStatus net = net_get_status(); const StreamStats stream = stream_get_stats(); char id[18]; device_id(id);
    json.object_open(); json.string("product", "LEDCAST"); json.string("id", id); json.string("name", cfg.wifi.hostname[0] ? cfg.wifi.hostname : net_hostname()); json.string("firmware", health.firmware_version);
    json.string("reset_reason", health.reset_reason); { PanicRecord panic{}; if (health_last_panic(panic)) { char hex[12]; json.object_open("last_panic"); json.integer("core", panic.core); snprintf(hex, sizeof(hex), "0x%08lx", static_cast<unsigned long>(panic.addr)); json.string("addr", hex); json.string("reason", panic.reason); json.array_open("backtrace"); for (uint8_t i = 0; i < panic.depth; ++i) { snprintf(hex, sizeof(hex), "0x%08lx", static_cast<unsigned long>(panic.backtrace[i])); json.string(nullptr, hex); } json.array_close(); json.object_close(); } }
    json.string("test_pattern", webui_test_pattern());
    json.boolean("show_enabled", cfg.show.enabled);
    json.boolean("show_control", true);
    json.integer("modules", cfg.line.module_count); json.integer("width", cfg.line.module.width * cfg.line.module_count); json.integer("height", cfg.line.module.height); json.integer("rssi", net.rssi); json.boolean("signal", stream.signal_present); json.integer("fps", stream.fps); json.string("owner", stream.source_ip); json.integer("bit_depth", cfg.display.bit_depth); json.integer("uptime_s", health.uptime_s);
    json.object_open("memory"); json.integer("free_heap", health.free_heap); json.integer("min_free_heap", health.min_free_heap); json.integer("largest_block", health.largest_free_block); json.object_close();
    json.object_open("network"); json.string("state", net_state_name(net.state)); json.string("hostname", net_hostname()); json.string("ssid", net.ssid); json.string("ip", net.ip); json.integer("rssi", net.rssi); json.string("ap_ssid", net.ap_ssid); json.string("ap_ip", net.ap_ip); json.integer("clients", net.sta_clients);
    json.integer("channel", net.channel); json.integer("tx_power_quarter_dbm", net.tx_power_quarter_dbm); json.integer("power_save", net.power_save);
    json.integer("last_disconnect_reason", net.disconnect_reason); json.object_close();
    json.object_open("display"); json.boolean("ready", display_is_ready()); json.integer("brightness", cfg.display.brightness); json.integer("bit_depth", cfg.display.bit_depth); json.integer("frame_rate", cfg.display.frame_rate);
    if (display_is_ready()) {
        const auto stats = display_get_stats();
        json.integer("fps", stats.fps_shown);
        json.integer("refresh_hz", stats.refresh_hz);
        json.integer("effective_bit_depth", stats.effective_bit_depth);
        json.integer("dma_bytes", stats.dma_bytes);
        json.integer("commit_us_avg", stats.commit_us_avg);
        json.integer("commit_us_max", stats.commit_us_max);
        json.integer("commit_over_budget", stats.commit_over_budget);
        json.integer("flip_failures", stats.flip_failures);
    }
    json.object_close();
    const uint32_t dropped = stream.header_invalid + stream.not_for_us +
                             stream.out_of_range + stream.assembly_overflows;
    json.object_open("stream"); json.boolean("signal", stream.signal_present); json.integer("fps", stream.fps); json.integer("source_fps", stream.source_fps); json.integer("received", stream.packets_received); json.integer("dropped", dropped);
    json.integer("header_invalid", stream.header_invalid); json.integer("not_for_us", stream.not_for_us); json.integer("out_of_range", stream.out_of_range);
    json.integer("frames_started", stream.frames_started); json.integer("frames_shown", stream.frames_shown); json.integer("frames_torn", stream.frames_torn); json.integer("frames_suppressed", stream.frames_suppressed); json.integer("frames_evicted", stream.frames_evicted); json.integer("frames_repeated", stream.frames_repeated); json.integer("frames_late", stream.frames_late); json.integer("frames_early", stream.frames_early); json.integer("reservoir_frames", stream.reservoir_frames); json.boolean("clock_synced", stream.clock_synced); json.integer("clock_rtt_us", stream.clock_rtt_us); json.integer("clock_samples", stream.clock_samples); json.integer("assembly_evictions", stream.assembly_evictions); json.integer("assembly_overflows", stream.assembly_overflows); json.integer("fec_recovered", stream.fec_recovered); json.integer("repeat_frames", stream.repeat_frames); json.integer("repeat_misses", stream.repeat_misses); json.integer("crc_or_decode_errors", stream.crc_or_decode_errors); json.integer("flip_timeouts", stream.flip_timeouts); json.integer("arrival_jitter_us_max", stream.arrival_jitter_us_max); json.integer("playout_ticks", stream.playout_ticks); json.integer("playout_fps", stream.playout_fps); json.integer("playout_jitter_us_max", stream.playout_jitter_us_max); json.integer("queue_depth", stream.queue_depth); json.integer("queue_depth_max", stream.queue_depth_max); json.integer("frame_age_us_avg", stream.frame_age_us_avg); json.integer("frame_age_us_max", stream.frame_age_us_max); json.integer("render_us_avg", stream.render_us_avg); json.integer("render_us_max", stream.render_us_max); json.integer("commit_interval_us_avg", stream.commit_interval_us_avg); json.integer("commit_jitter_us_max", stream.commit_jitter_us_max); json.string("owner", stream.source_ip); json.string("source_ip", stream.source_ip); json.integer("last_frame_ms", stream.last_packet_us > 0 ? (esp_timer_get_time() - stream.last_packet_us) / 1000 : -1); json.object_close();
    json.object_close();
}
esp_err_t handle_status(httpd_req_t* req) { char buffer[kStatusSinkBuffer]; JsonSink json = make_http_sink(req, buffer, sizeof(buffer)); write_status(json); return finish_http_sink(req, json); }
}  // namespace ledcast::webui
