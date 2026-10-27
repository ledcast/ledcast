#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ledcast::captive {
// One uncompressed question only; no dynamic allocation or pointer traversal.
// Other record types receive NODATA so IPv6 clients retry the IPv4 address.
inline size_t dns_reply(uint8_t* packet, size_t length, size_t capacity,
                        const uint8_t address[4]) {
    if (length < 12 || length > 512 || capacity < length ||
        (packet[2] & 0xf8) || packet[3] != 0 || packet[4] || packet[5] != 1 ||
        packet[6] || packet[7] || packet[8] || packet[9]) return 0;
    size_t end = 12;
    while (true) {
        if (end >= length) return 0;
        const uint8_t label = packet[end++];
        if (!label) break;
        if (label > 63 || label > length - end) return 0;
        end += label;
        if (end - 12 > 254) return 0;
    }
    if (length - end < 4) return 0;
    const bool ipv4 = packet[end] == 0 && packet[end + 1] == 1;
    if (packet[end + 2] != 0 || packet[end + 3] != 1) return 0;
    end += 4;
    if (ipv4 && capacity - end < 16) return 0;
    packet[2] = 0x80 | (packet[2] & 1); // response, preserve recursion desired
    packet[3] = 0;
    packet[7] = ipv4 ? 1 : 0;
    packet[10] = packet[11] = 0; // omit optional EDNS records
    if (!ipv4) return end;
    const uint8_t answer[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 0, 0, 4};
    memcpy(packet + end, answer, sizeof(answer));
    memcpy(packet + end + sizeof(answer), address, 4);
    return end + 16;
}
} // namespace ledcast::captive
