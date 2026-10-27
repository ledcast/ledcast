#pragma once
#include <cstdint>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

namespace ledcast::webui {
// HTTPD uses an IPv6 dual-stack listener when LWIP_IPV6 is enabled. IPv4
// connections then have IPv4-mapped socket addresses, not AF_INET addresses.
// IPv4-адрес сокета (в том числе отображённый в IPv6) в сетевом порядке байт.
inline bool socket_ipv4(const sockaddr_storage& socket, uint32_t& address) {
    if (socket.ss_family == AF_INET) {
        address = reinterpret_cast<const sockaddr_in*>(&socket)->sin_addr.s_addr;
        return true;
    }
    if (socket.ss_family != AF_INET6) return false;
    const auto& ipv6 = reinterpret_cast<const sockaddr_in6*>(&socket)->sin6_addr;
    static constexpr unsigned char prefix[12] = {0,0,0,0,0,0,0,0,0,0,255,255};
    if (std::memcmp(ipv6.s6_addr, prefix, sizeof(prefix)) != 0) return false;
    std::memcpy(&address, ipv6.s6_addr + 12, sizeof(address));
    return true;
}
inline bool socket_has_ipv4(const sockaddr_storage& socket, const char* expected) {
    in_addr address{};
    if (!expected || inet_pton(AF_INET, expected, &address) != 1) return false;
    if (socket.ss_family == AF_INET)
        return reinterpret_cast<const sockaddr_in*>(&socket)->sin_addr.s_addr == address.s_addr;
    if (socket.ss_family != AF_INET6) return false;
    const auto& ipv6 = reinterpret_cast<const sockaddr_in6*>(&socket)->sin6_addr;
    static constexpr unsigned char prefix[12] = {0,0,0,0,0,0,0,0,0,0,255,255};
    return std::memcmp(ipv6.s6_addr, prefix, sizeof(prefix)) == 0 &&
           std::memcmp(ipv6.s6_addr + 12, &address.s_addr, sizeof(address.s_addr)) == 0;
}
}
