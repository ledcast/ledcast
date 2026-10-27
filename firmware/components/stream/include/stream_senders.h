#pragma once

#include <cstddef>
#include <cstdint>

namespace ledcast::stream {

// Адреса, с которых строка принимает поток: только те, что вошли паролем строки
// или ключом Console. Срок совпадает с сессией входа; повторный вход продлевает его.
class SenderList {
public:
    static constexpr size_t kSlots = 4;

    // Занимает ячейку этого адреса, свободную или ту, что истекает раньше всех.
    void allow(uint32_t ipv4, int64_t until_us) {
        Slot* target = &slots_[0];
        for (Slot& slot : slots_) {
            if (slot.ipv4 == ipv4) {
                target = &slot;
                break;
            }
            if (slot.until_us < target->until_us) target = &slot;
        }
        if (target->ipv4 != ipv4 || target->until_us < until_us) target->until_us = until_us;
        target->ipv4 = ipv4;
    }

    bool allowed(uint32_t ipv4, int64_t now_us) const {
        if (ipv4 == 0) return false;
        for (const Slot& slot : slots_) {
            if (slot.ipv4 == ipv4 && now_us < slot.until_us) return true;
        }
        return false;
    }

private:
    struct Slot {
        uint32_t ipv4 = 0;
        int64_t until_us = 0;
    };
    Slot slots_[kSlots];
};

}  // namespace ledcast::stream
