#include "../components/netconfig/captive_dns.h"
#include <array>
#include "check.h"
#include <cstdio>

int main() {
    constexpr uint8_t address[] = {192,168,4,1};
    const std::array<uint8_t, 19> query{0x12,0x34,1,0,0,1,0,0,0,0,0,0,1,'a',0,0,1,0,1};
    std::array<uint8_t, 528> packet{};
    auto reset = [&] { packet.fill(0); std::memcpy(packet.data(),query.data(),query.size()); };
    reset();
    CHECK(ledcast::captive::dns_reply(packet.data(),19,packet.size(),address)==35);
    CHECK(packet[0]==0x12 && packet[1]==0x34 && packet[2]==0x81 && packet[7]==1);
    CHECK(std::memcmp(packet.data()+31,address,4)==0);
    for (size_t length=0; length<19; ++length) {
        reset(); CHECK(ledcast::captive::dns_reply(packet.data(),length,packet.size(),address)==0);
    }
    reset(); packet[16]=28; // AAAA: valid NODATA, never bogus IPv6 answer.
    CHECK(ledcast::captive::dns_reply(packet.data(),19,packet.size(),address)==19 && packet[7]==0);
    reset(); packet[12]=0xc0; packet[13]=12; // self-referencing compression
    CHECK(ledcast::captive::dns_reply(packet.data(),19,packet.size(),address)==0);
    reset(); packet[5]=2;
    CHECK(ledcast::captive::dns_reply(packet.data(),19,packet.size(),address)==0);
    reset(); packet[18]=3;
    CHECK(ledcast::captive::dns_reply(packet.data(),19,packet.size(),address)==0);
    reset(); packet[2]=0x81;
    CHECK(ledcast::captive::dns_reply(packet.data(),19,packet.size(),address)==0);
    reset();
    CHECK(ledcast::captive::dns_reply(packet.data(),19,34,address)==0);
    CHECK(ledcast::captive::dns_reply(packet.data(),513,packet.size(),address)==0);
    // Deterministic malformed input sweep under ASan/UBSan.
    uint32_t seed=7;
    for (size_t n=0; n<100000; ++n) {
        for (auto& byte:packet) { seed=seed*1664525+1013904223; byte=seed>>24; }
        const size_t length=n%529;
        const size_t reply=ledcast::captive::dns_reply(packet.data(),length,packet.size(),address);
        CHECK(reply<=packet.size());
    }
    if (!hosttest::failures) std::puts("captive DNS tests passed");
    return hosttest::failures ? 1 : 0;
}
