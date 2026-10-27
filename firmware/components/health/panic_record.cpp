// Причина последней паники переживает перезагрузку: последовательного порта на
// площадке нет, а без адреса и стека вызовов падение не найти. Обработчик
// паники ESP-IDF обёрнут (-Wl,--wrap=esp_panic_handler): запись делается до
// штатного разбора и перезагрузки.

#include <cstring>

#include <esp_attr.h>
#include <esp_cpu_utils.h>
#include <esp_debug_helpers.h>
#include <esp_private/panic_internal.h>
#include <xtensa_context.h>

#include "health.h"

namespace ledcast {
namespace {

constexpr uint32_t kMagic = 0x4c435031;  // "LCP1"

struct Record {
    uint32_t magic;
    PanicRecord panic;
    uint32_t checksum;
};

RTC_NOINIT_ATTR Record s_record;

IRAM_ATTR uint32_t checksum(const PanicRecord& panic) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&panic);
    uint32_t sum = 0x811c9dc5u;
    for (size_t i = 0; i < sizeof(panic); ++i) sum = (sum ^ bytes[i]) * 0x01000193u;
    return sum;
}

IRAM_ATTR void copy_text(char* out, size_t size, const char* text) {
    size_t i = 0;
    if (text != nullptr) {
        for (; i + 1 < size && text[i] != '\0'; ++i) out[i] = text[i];
    }
    out[i] = '\0';
}

}  // namespace

bool health_last_panic(PanicRecord& out) {
    if (s_record.magic != kMagic || s_record.checksum != checksum(s_record.panic)) return false;
    out = s_record.panic;
    out.reason[sizeof(out.reason) - 1] = '\0';
    return true;
}

}  // namespace ledcast

extern "C" void __real_esp_panic_handler(panic_info_t* info);

extern "C" IRAM_ATTR void __wrap_esp_panic_handler(panic_info_t* info) {
    using ledcast::s_record;
    std::memset(&s_record, 0, sizeof(s_record));
    ledcast::PanicRecord& panic = s_record.panic;
    panic.core = static_cast<uint8_t>(info->core);
    panic.addr = reinterpret_cast<uint32_t>(info->addr);
    ledcast::copy_text(panic.reason, sizeof(panic.reason),
                       g_panic_abort ? g_panic_abort_details : info->reason);
    if (info->frame != nullptr) {
        const auto* exc = static_cast<const XtExcFrame*>(info->frame);
        esp_backtrace_frame_t frame = {.pc = static_cast<uint32_t>(exc->pc),
                                       .sp = static_cast<uint32_t>(exc->a1),
                                       .next_pc = static_cast<uint32_t>(exc->a0),
                                       .exc_frame = exc};
        panic.backtrace[panic.depth++] = esp_cpu_process_stack_pc(frame.pc);
        while (panic.depth < sizeof(panic.backtrace) / sizeof(panic.backtrace[0]) &&
               frame.next_pc != 0 && esp_backtrace_get_next_frame(&frame)) {
            panic.backtrace[panic.depth++] = esp_cpu_process_stack_pc(frame.pc);
        }
    }
    s_record.checksum = ledcast::checksum(panic);
    s_record.magic = ledcast::kMagic;
    __real_esp_panic_handler(info);
}
