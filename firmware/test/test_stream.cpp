#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "check.h"
#include "stream_clock.h"
#include "stream_codec.h"
#include "stream_frame.h"
#include "runtime_heap_budget.h"
#include "stream_packet.h"
#include "stream_senders.h"

using namespace ledcast::stream;

namespace {

void put16(uint8_t* out, uint16_t value) {
    out[0] = static_cast<uint8_t>(value >> 8);
    out[1] = static_cast<uint8_t>(value);
}
void put32(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value >> 24);
    out[1] = static_cast<uint8_t>(value >> 16);
    out[2] = static_cast<uint8_t>(value >> 8);
    out[3] = static_cast<uint8_t>(value);
}
void put64(uint8_t* out, uint64_t value) {
    put32(out, static_cast<uint32_t>(value >> 32));
    put32(out + 4, static_cast<uint32_t>(value));
}

Header make_header(uint32_t frame_id, uint32_t encoded_bytes, uint16_t width,
                   uint16_t height, FrameCodec codec = FrameCodec::Raw) {
    Header h{};
    h.kind = PacketKind::Data;
    h.codec = codec;
    h.flags = kFrameFlagFecPresent;
    h.session_id = 0x0102030405060708ULL;
    h.stream_id = 0x11223344;
    h.frame_id = frame_id;
    h.pts_ns = 1234567890123456ULL + frame_id * 20000000ULL;
    h.rate_n = 60000;
    h.rate_d = 1001;
    h.width = width;
    h.height = height;
    h.decoded_bytes = static_cast<uint32_t>(width) * height * 3u;
    h.encoded_bytes = encoded_bytes;
    h.frame_crc32 = 0xa1b2c3d4;
    h.data_chunk_count = static_cast<uint16_t>(
        (encoded_bytes + kChunkPayloadSize - 1u) / kChunkPayloadSize);
    h.chunk_payload_size = kChunkPayloadSize;
    h.clock_port = 50123;
    return h;
}

std::vector<uint8_t> wire_packet(const Header& header, const uint8_t* payload,
                                 uint16_t payload_len) {
    std::vector<uint8_t> packet(kFrameHeaderLen + payload_len, 0);
    std::memcpy(packet.data(), kFrameMagic, sizeof(kFrameMagic));
    packet[4] = kFrameVersion;
    packet[5] = static_cast<uint8_t>(header.kind);
    packet[6] = static_cast<uint8_t>(header.codec);
    packet[7] = header.flags;
    put16(packet.data() + 8, kFrameHeaderLen);
    put16(packet.data() + 10, payload_len);
    put64(packet.data() + 12, header.session_id);
    put32(packet.data() + 20, header.stream_id);
    put32(packet.data() + 24, header.frame_id);
    put64(packet.data() + 28, header.pts_ns);
    put32(packet.data() + 36, header.rate_n);
    put32(packet.data() + 40, header.rate_d);
    put16(packet.data() + 44, header.width);
    put16(packet.data() + 46, header.height);
    put32(packet.data() + 48, header.decoded_bytes);
    put32(packet.data() + 52, header.encoded_bytes);
    put32(packet.data() + 56, header.frame_crc32);
    put16(packet.data() + 60, header.chunk_index);
    put16(packet.data() + 62, header.data_chunk_count);
    put16(packet.data() + 64, header.chunk_payload_size);
    put16(packet.data() + 66, header.clock_port);
    put32(packet.data() + 68, crc32(packet.data(), 68));
    if (payload_len != 0) std::memcpy(packet.data() + kFrameHeaderLen, payload, payload_len);
    return packet;
}

Header parse_chunk(const Header& frame, PacketKind kind, uint16_t index,
                   const uint8_t* payload, uint16_t length,
                   std::vector<uint8_t>& packet) {
    Header chunk = frame;
    chunk.kind = kind;
    chunk.chunk_index = index;
    packet = wire_packet(chunk, payload, length);
    Header parsed{};
    CHECK(packet_parse(packet.data(), packet.size(), parsed));
    return parsed;
}

void test_crc_golden() {
    static constexpr uint8_t input[] = {'1','2','3','4','5','6','7','8','9'};
    CHECK_EQ(crc32(input, sizeof(input)), 0xcbf43926u);
}

void test_v6_header_golden_and_strictness() {
    std::array<uint8_t, 1200> payload{};
    Header source = make_header(0x55667788, 2304, 32, 24, FrameCodec::Lz4);
    source.chunk_index = 0;
    auto packet = wire_packet(source, payload.data(), payload.size());
    Header parsed{};
    CHECK_EQ(packet.size(), kFrameHeaderLen + payload.size());
    CHECK(std::memcmp(packet.data(), "LCF6", 4) == 0);
    CHECK_EQ(packet[4], 6);
    CHECK_EQ(packet[5], 0);
    CHECK_EQ(packet[6], 2);
    CHECK_EQ(packet[7], 1);
    CHECK_EQ(packet[8], 0);
    CHECK_EQ(packet[9], 72);
    CHECK(packet_parse(packet.data(), packet.size(), parsed));
    CHECK_EQ(parsed.session_id, 0x0102030405060708ULL);
    CHECK_EQ(parsed.stream_id, 0x11223344u);
    CHECK_EQ(parsed.frame_id, 0x55667788u);
    CHECK_EQ(parsed.rate_n, 60000u);
    CHECK_EQ(parsed.rate_d, 1001u);
    CHECK_EQ(parsed.chunk_payload_size, 1200);
    CHECK_EQ(parsed.clock_port, 50123);

    auto bad = packet;
    bad[24] ^= 1;
    CHECK(!packet_parse(bad.data(), bad.size(), parsed));
    bad = packet;
    bad[7] = 3;
    put32(bad.data() + 68, crc32(bad.data(), 68));
    CHECK(!packet_parse(bad.data(), bad.size(), parsed));
    bad = packet;
    bad[7] = 0;
    put32(bad.data() + 68, crc32(bad.data(), 68));
    CHECK(!packet_parse(bad.data(), bad.size(), parsed));
    bad = packet;
    put16(bad.data() + 66, 0);  // без порта часов кадр не показать вовремя
    put32(bad.data() + 68, crc32(bad.data(), 68));
    CHECK(!packet_parse(bad.data(), bad.size(), parsed));
    bad = packet;
    bad[0] = 'L'; bad[1] = 'D'; bad[2] = 'C'; bad[3] = 'F';
    put32(bad.data() + 68, crc32(bad.data(), 68));
    CHECK(!packet_parse(bad.data(), bad.size(), parsed));
    CHECK(!packet_parse(packet.data(), packet.size() - 1, parsed));
}

void test_chunk_geometry_is_exact() {
    std::array<uint8_t, 1200> payload{};
    Header frame = make_header(1, 2500, 10, 10, FrameCodec::Lz4);
    frame.chunk_index = 2;
    auto last = wire_packet(frame, payload.data(), 100);
    Header parsed{};
    CHECK(packet_parse(last.data(), last.size(), parsed));

    auto wrong = wire_packet(frame, payload.data(), 101);
    CHECK(!packet_parse(wrong.data(), wrong.size(), parsed));
    frame.kind = PacketKind::Parity;
    frame.chunk_index = frame.data_chunk_count;
    auto parity = wire_packet(frame, payload.data(), 1200);
    CHECK(packet_parse(parity.data(), parity.size(), parsed));
    frame.chunk_index = 0;
    auto bad_parity = wire_packet(frame, payload.data(), 1200);
    CHECK(!packet_parse(bad_parity.data(), bad_parity.size(), parsed));
}

std::vector<uint8_t> make_data(size_t bytes) {
    std::vector<uint8_t> data(bytes);
    uint32_t seed = 0x12345678;
    for (uint8_t& value : data) {
        seed = seed * 1664525u + 1013904223u;
        value = static_cast<uint8_t>(seed >> 24);
    }
    return data;
}

std::array<uint8_t, kChunkPayloadSize> parity_for(const std::vector<uint8_t>& data) {
    std::array<uint8_t, kChunkPayloadSize> parity{};
    for (size_t offset = 0; offset < data.size(); offset += kChunkPayloadSize) {
        const size_t length = std::min<size_t>(kChunkPayloadSize, data.size() - offset);
        for (size_t i = 0; i < length; ++i) parity[i] ^= data[offset + i];
    }
    return parity;
}

void test_reorder_duplicate_and_conflict() {
    auto data = make_data(2500);
    Header frame = make_header(10, data.size(), 50, 20, FrameCodec::Lz4);
    std::vector<uint8_t> storage(frame_slot_bytes(frame.decoded_bytes));
    FrameAssembly assembly{};
    frame_init(assembly, storage.data(), storage.size());
    CHECK(frame_begin(assembly, frame, 100));
    // LZ4 ложится в конец слота, чтобы распаковаться в его начало.
    CHECK(assembly.encoded == storage.data() + storage.size() - data.size());
    std::vector<uint8_t> packet;

    Header c2 = parse_chunk(frame, PacketKind::Data, 2, data.data() + 2400, 100, packet);
    CHECK_EQ(frame_accept(assembly, c2), ChunkResult::Stored);
    CHECK_EQ(frame_accept(assembly, c2), ChunkResult::Duplicate);

    auto conflict_data = std::vector<uint8_t>(data.begin() + 2400, data.end());
    conflict_data[0] ^= 0xff;
    Header conflict = parse_chunk(frame, PacketKind::Data, 2, conflict_data.data(), 100, packet);
    CHECK_EQ(frame_accept(assembly, conflict), ChunkResult::Conflict);

    Header c0 = parse_chunk(frame, PacketKind::Data, 0, data.data(), 1200, packet);
    CHECK_EQ(frame_accept(assembly, c0), ChunkResult::Stored);
    Header c1 = parse_chunk(frame, PacketKind::Data, 1, data.data() + 1200, 1200, packet);
    CHECK_EQ(frame_accept(assembly, c1), ChunkResult::Complete);
    CHECK(std::equal(data.begin(), data.end(), assembly.encoded));
    CHECK_EQ(frame_accept(assembly, c1), ChunkResult::Duplicate);
    const auto parity = parity_for(data);
    Header late_parity = parse_chunk(frame, PacketKind::Parity, frame.data_chunk_count,
                                     parity.data(), parity.size(), packet);
    CHECK_EQ(frame_accept(assembly, late_parity), ChunkResult::Duplicate);
}

void test_single_loss_parity_recovery() {
    auto data = make_data(2500);
    auto parity = parity_for(data);
    Header frame = make_header(11, data.size(), 50, 20, FrameCodec::Lz4);
    std::vector<uint8_t> storage(frame_slot_bytes(frame.decoded_bytes), 0);
    std::array<uint8_t, kChunkPayloadSize> parity_buffer{};
    FrameAssembly assembly{};
    frame_init(assembly, storage.data(), storage.size());
    CHECK(frame_begin(assembly, frame, 100, parity_buffer.data()));
    std::vector<uint8_t> packet;

    Header c2 = parse_chunk(frame, PacketKind::Data, 2, data.data() + 2400, 100, packet);
    CHECK_EQ(frame_accept(assembly, c2), ChunkResult::Stored);
    Header parity_chunk = parse_chunk(frame, PacketKind::Parity, frame.data_chunk_count,
                                      parity.data(), parity.size(), packet);
    CHECK_EQ(frame_accept(assembly, parity_chunk), ChunkResult::Stored);
    Header c0 = parse_chunk(frame, PacketKind::Data, 0, data.data(), 1200, packet);
    CHECK_EQ(frame_accept(assembly, c0), ChunkResult::Recovered);
    CHECK(std::equal(data.begin(), data.end(), assembly.encoded));
    CHECK(assembly.parity == nullptr);  // the buffer returns to the pool

    // Without a pool buffer the frame still assembles, only without FEC.
    FrameAssembly bare{};
    frame_init(bare, storage.data(), storage.size());
    CHECK(frame_begin(bare, frame, 100));
    Header bare_parity = parse_chunk(frame, PacketKind::Parity, frame.data_chunk_count,
                                     parity.data(), parity.size(), packet);
    CHECK_EQ(frame_accept(bare, bare_parity), ChunkResult::Duplicate);
}

void test_two_slots_and_dropped_frame_isolation() {
    auto a = make_data(2400);
    auto b = make_data(2500);
    b[0] ^= 0x55;
    Header ah = make_header(20, a.size(), 50, 20, FrameCodec::Lz4);
    Header bh = make_header(21, b.size(), 50, 20, FrameCodec::Lz4);
    std::vector<uint8_t> as(frame_slot_bytes(ah.decoded_bytes)), bs(frame_slot_bytes(bh.decoded_bytes));
    FrameAssembly slots[2]{};
    frame_init(slots[0], as.data(), as.size());
    frame_init(slots[1], bs.data(), bs.size());
    CHECK(frame_begin(slots[0], ah, 10));
    CHECK(frame_begin(slots[1], bh, 11));
    std::vector<uint8_t> packet;

    Header a0 = parse_chunk(ah, PacketKind::Data, 0, a.data(), 1200, packet);
    CHECK_EQ(frame_accept(slots[0], a0), ChunkResult::Stored);
    Header b1 = parse_chunk(bh, PacketKind::Data, 1, b.data() + 1200, 1200, packet);
    CHECK_EQ(frame_accept(slots[1], b1), ChunkResult::Stored);
    frame_reset(slots[0]);
    Header b0 = parse_chunk(bh, PacketKind::Data, 0, b.data(), 1200, packet);
    CHECK_EQ(frame_accept(slots[1], b0), ChunkResult::Stored);
    Header b2 = parse_chunk(bh, PacketKind::Data, 2, b.data() + 2400, 100, packet);
    CHECK_EQ(frame_accept(slots[1], b2), ChunkResult::Complete);
    CHECK(std::equal(b.begin(), b.end(), slots[1].encoded));
}

void test_frame_decode() {
    const std::vector<uint8_t> raw = {1, 2, 3, 4, 5, 6};
    std::vector<uint8_t> out(raw.size());
    CHECK(decode_frame(FrameCodec::Raw, raw.data(), raw.size(), out.data(), out.size()));
    CHECK(out == raw);
    CHECK(!decode_frame(FrameCodec::Raw, raw.data(), raw.size() - 1, out.data(), out.size()));

    const std::vector<uint8_t> lz4 = {0x60, 1, 2, 3, 4, 5, 6};
    std::fill(out.begin(), out.end(), 0);
    CHECK(decode_frame(FrameCodec::Lz4, lz4.data(), lz4.size(), out.data(), out.size()));
    CHECK(out == raw);
    const std::vector<uint8_t> bad = {0x00, 0x00, 0x00};
    CHECK(!decode_frame(FrameCodec::Lz4, bad.data(), bad.size(), out.data(), out.size()));
}

// LZ4 в конце слота распаковывается в его начало: и сжимаемый кадр, и кадр из
// одних литералов, который длиннее распакованного.
void test_lz4_decodes_in_place() {
    constexpr uint32_t decoded = 3000;
    std::vector<uint8_t> expected(decoded);
    for (uint32_t i = 0; i < decoded - 5; ++i) expected[i] = static_cast<uint8_t>(1 + i % 3);
    std::fill(expected.end() - 5, expected.end(), 9);
    std::vector<uint8_t> block = {0x3f, 1, 2, 3, 0x03, 0x00};
    block.insert(block.end(), 11, 255);
    block.push_back(168);  // match 4 + 15 + 11 * 255 + 168 = 2992
    block.push_back(0x50);
    block.insert(block.end(), 5, 9);

    std::vector<uint8_t> slot(frame_slot_bytes(decoded), 0xee);
    uint8_t* encoded = slot.data() + slot.size() - block.size();
    std::copy(block.begin(), block.end(), encoded);
    CHECK(decode_frame(FrameCodec::Lz4, encoded, block.size(), slot.data(), decoded));
    CHECK(std::equal(expected.begin(), expected.end(), slot.begin()));

    std::vector<uint8_t> literals = {0xf0};
    literals.insert(literals.end(), 11, 255);
    literals.push_back(180);  // 15 + 11 * 255 + 180 = 3000 literals
    for (uint32_t i = 0; i < decoded; ++i) literals.push_back(static_cast<uint8_t>(i * 7));
    CHECK(literals.size() <= frame_slot_bytes(decoded));
    std::fill(slot.begin(), slot.end(), 0xee);
    encoded = slot.data() + slot.size() - literals.size();
    std::copy(literals.begin(), literals.end(), encoded);
    CHECK(decode_frame(FrameCodec::Lz4, encoded, literals.size(), slot.data(), decoded));
    for (uint32_t i = 0; i < decoded; ++i) CHECK_EQ(slot[i], static_cast<uint8_t>(i * 7));

    // Raw frames already lie where they are shown.
    CHECK(decode_frame(FrameCodec::Raw, slot.data(), decoded, slot.data(), decoded));
}

void test_short_frame_parity_matches_its_length() {
    auto data = make_data(300);
    Header frame = make_header(12, data.size(), 50, 20, FrameCodec::Lz4);
    frame.kind = PacketKind::Parity;
    frame.chunk_index = frame.data_chunk_count;
    std::array<uint8_t, kChunkPayloadSize> padded{};
    std::memcpy(padded.data(), data.data(), data.size());
    Header parsed{};
    auto full = wire_packet(frame, padded.data(), kChunkPayloadSize);
    CHECK(!packet_parse(full.data(), full.size(), parsed));

    // Единственный фрагмент теряется целиком: чётность и есть его копия.
    frame.kind = PacketKind::Data;
    frame.chunk_index = 0;
    std::vector<uint8_t> storage(frame_slot_bytes(frame.decoded_bytes), 0);
    std::array<uint8_t, kChunkPayloadSize> parity_buffer{};
    FrameAssembly assembly{};
    frame_init(assembly, storage.data(), storage.size());
    CHECK(frame_begin(assembly, frame, 100, parity_buffer.data()));
    std::vector<uint8_t> packet;
    Header parity = parse_chunk(frame, PacketKind::Parity, frame.data_chunk_count,
                                data.data(), data.size(), packet);
    CHECK_EQ(frame_accept(assembly, parity), ChunkResult::Recovered);
    CHECK(std::equal(data.begin(), data.end(), assembly.encoded));
    CHECK_EQ(frame_accept(assembly, parity), ChunkResult::Duplicate);
}

void test_repeat_frame() {
    const uint8_t reference[kRepeatPayloadLen] = {0x01, 0x02, 0x03, 0x04};
    Header frame = make_header(13, kRepeatPayloadLen, 50, 20, FrameCodec::Repeat);
    auto packet = wire_packet(frame, reference, kRepeatPayloadLen);
    Header parsed{};
    CHECK(packet_parse(packet.data(), packet.size(), parsed));
    uint32_t frame_id = 0;
    CHECK(repeat_reference(parsed.data, parsed.encoded_bytes, frame_id));
    CHECK_EQ(frame_id, 0x01020304u);
    CHECK(!repeat_reference(parsed.data, kRepeatPayloadLen - 1, frame_id));

    Header longer = make_header(14, kRepeatPayloadLen + 1, 50, 20, FrameCodec::Repeat);
    const uint8_t five[5] = {};
    auto bad = wire_packet(longer, five, sizeof(five));
    CHECK(!packet_parse(bad.data(), bad.size(), parsed));
    std::vector<uint8_t> out(frame.decoded_bytes);
    CHECK(!decode_frame(FrameCodec::Repeat, reference, sizeof(reference), out.data(), out.size()));
}

void test_frame_serial_wrap() {
    CHECK(frame_newer(1, 0));
    CHECK(frame_newer(0, 0xffffffffu));
    CHECK(!frame_newer(9, 10));
    CHECK(!frame_newer(0x80000000u, 0));
}

void test_delayed_frames_cannot_cross_committed_boundary() {
    // No commit yet: even zero is a valid first frame. After commit, neither
    // its duplicate nor an old frame outside the eight-entry cache can enter.
    CHECK(!frame_obsolete(0, false, 0));
    CHECK(!frame_obsolete(24, false, 24));
    CHECK(frame_obsolete(24, true, 24));
    CHECK(frame_obsolete(15, true, 24));
    CHECK(!frame_obsolete(25, true, 24));
    CHECK(!frame_obsolete(0, true, 0xffffffffu));
    CHECK(frame_obsolete(0xfffffff8u, true, 2));
    CHECK(frame_obsolete(0xffffffffu, true, 2));
}

void test_bounded_jitter_reservoir() {
    CHECK_EQ(kMinFrameSlots, 4);
    CHECK_EQ(kMaxFrameSlots, 20);
    CHECK_EQ(reservoir_frames(kMinFrameSlots), 3);
    CHECK_EQ(reservoir_frames(kMaxFrameSlots), 19);
    // Ten modules 32 x 16: 15 KiB per frame plus the in-place LZ4 margin.
    CHECK_EQ(frame_slot_bytes(15360), 15452u);
    size_t pool_bytes = 0;
    CHECK(frame_pool_bytes(15360, 5, pool_bytes));
    CHECK_EQ(pool_bytes, 77260u);
    CHECK(!frame_pool_bytes(0, 5, pool_bytes));
    CHECK(!frame_pool_bytes(15360, 0, pool_bytes));
    // Live resize: all unrelated services already exist, reserve only future
    // worker stacks/TCBs plus the shared control-plane headroom.
    constexpr size_t pending_tasks = 15 * 1024;
    constexpr size_t two_frames_pool = 32 * 16 * 3 * 2 * kMinFrameSlots;
    const size_t exact_budget = two_frames_pool + pending_tasks +
        ledcast::runtime_memory::kServiceHeadroom;
    CHECK(ledcast::runtime_memory::frame_allocation_fits(exact_budget,3072,3072,
                                                        two_frames_pool,pending_tasks));
    CHECK(!ledcast::runtime_memory::frame_allocation_fits(exact_budget-1,3072,3072,
                                                         two_frames_pool,pending_tasks));
    CHECK(!ledcast::runtime_memory::frame_allocation_fits(exact_budget,3071,3072,
                                                         two_frames_pool,pending_tasks));
    CHECK(!ledcast::runtime_memory::frame_allocation_fits(0,3072,3072,two_frames_pool,pending_tasks));
    CHECK(!ledcast::runtime_memory::frame_allocation_fits(SIZE_MAX,3072,3072,
                                                         SIZE_MAX,pending_tasks));

    std::array<FrameAssembly, 3> slots{};
    for (size_t i = 0; i < slots.size(); ++i) {
        slots[i].header = make_header(static_cast<uint32_t>(i + 2), 6, 2, 1);
        slots[i].state = AssemblyState::Complete;
    }
    const auto session = slots[0].header.session_id;
    const auto stream = slots[0].header.stream_id;
    CHECK_EQ(frame_complete_count(slots.data(), slots.size(), session, stream), slots.size());
    slots[0].state = AssemblyState::Receiving;
    CHECK_EQ(frame_complete_count(slots.data(), slots.size(), session, stream), slots.size() - 1);
    slots[0].state = AssemblyState::Complete;
    for (size_t i = 0; i < slots.size(); ++i) {
        FrameAssembly* input = frame_next_decode(slots.data(), slots.size());
        CHECK(input == &slots[i]);
        input->state = AssemblyState::Decoding;
        frame_reset(*input);
        CHECK_EQ(input->state, AssemblyState::Empty);
    }
    CHECK(frame_next_decode(slots.data(), slots.size()) == nullptr);
}

void test_playout_tolerates_timer_boundary_jitter() {
    constexpr uint64_t period_ns = 16666666;
    uint32_t last_shown = 0;
    for (uint32_t tick = 1; tick <= 120; ++tick) {
        const uint64_t due = tick * period_ns + (tick % 2 ? -50000LL : 50000LL);
        uint32_t selected = last_shown;
        for (uint32_t candidate = last_shown + 1; candidate <= tick + 1; ++candidate) {
            if (frame_due(candidate * period_ns, due, 16667)) selected = candidate;
        }
        CHECK_EQ(selected, tick); // Neither a repeat nor a skipped frame.
        last_shown = selected;
    }
    CHECK(frame_due(10000000, 0, 1000000));
    CHECK(!frame_due(10000001, 0, 1000000)); // Margin is bounded to 10ms.
    CHECK(!frame_due(period_ns, 0, 16667)); // Never take the next whole frame.
}

void put_clock_reply(uint8_t* out, uint64_t session, int64_t t1_us, uint64_t t2_ns,
                     uint64_t t3_ns) {
    clock_request_write(out, session, t1_us);
    out[5] = static_cast<uint8_t>(ClockKind::Reply);
    put64(out + 24, t2_ns);
    put64(out + 32, t3_ns);
}

void test_clock_packets() {
    uint8_t packet[kClockPacketLen];
    clock_request_write(packet, 0x0102030405060708ULL, 123456789);
    CHECK(std::memcmp(packet, "LCK6", 4) == 0);
    CHECK_EQ(packet[4], 6);
    CHECK_EQ(packet[5], 0);
    ClockReply reply{};
    CHECK(!clock_reply_parse(packet, sizeof(packet), reply));  // a request is not a reply
    put_clock_reply(packet, 0x0102030405060708ULL, 123456789, 5000000000ULL, 5000000300ULL);
    CHECK(clock_reply_parse(packet, sizeof(packet), reply));
    CHECK_EQ(reply.session_id, 0x0102030405060708ULL);
    CHECK_EQ(reply.t1_us, 123456789);
    CHECK_EQ(reply.t2_ns, 5000000000ULL);
    CHECK_EQ(reply.t3_ns, 5000000300ULL);
    CHECK(!clock_reply_parse(packet, sizeof(packet) - 1, reply));
    packet[4] = 5;
    CHECK(!clock_reply_parse(packet, sizeof(packet), reply));
}

// Console на 7 с впереди; путь до неё 2 мс туда и 1 мс обратно, а Wi‑Fi
// иногда держит пакет в очереди.
void test_sender_clock_prefers_shortest_exchange() {
    constexpr int64_t kOffsetNs = 7000000000LL;
    SenderClock clock;
    CHECK(!clock.synced());
    auto exchange = [&](int64_t t1_us, int64_t forward_us, int64_t back_us) {
        const uint64_t t2 = static_cast<uint64_t>((t1_us + forward_us) * 1000 + kOffsetNs);
        const uint64_t t3 = t2 + 150000;  // Console отвечает за 150 мкс
        return clock.add(t1_us, t2, t3, t1_us + forward_us + 150 + back_us);
    };
    CHECK(exchange(1000000, 40000, 1000));  // queued forward: 19.5 ms off
    CHECK(exchange(1250000, 2000, 1000));
    CHECK(exchange(1500000, 1000, 30000));
    CHECK(!clock.synced());
    CHECK(exchange(1750000, 9000, 9000));
    CHECK(clock.synced());
    CHECK_EQ(clock.rtt_us(), 3000u);
    // Offset error is half the path asymmetry: 0.5 ms.
    CHECK_EQ(clock.sender_ns(2000000) - (2000000LL * 1000 + kOffsetNs), 500000);

    // Answers that went backwards in time or waited too long are not samples.
    CHECK(!clock.add(3000000, 10, 5, 3000100));
    CHECK(!clock.add(3000000, 5, 10, 2999999));
    CHECK(!exchange(3250000, 150000, 100000));
    CHECK_EQ(clock.samples(), 4);

    // The window forgets the best exchange once it is 16 probes old.
    for (int i = 0; i < SenderClock::kWindow; ++i) CHECK(exchange(4000000 + i * 250000, 3000, 3000));
    CHECK_EQ(clock.rtt_us(), 6000u);
    CHECK_EQ(clock.sender_ns(9000000) - (9000000LL * 1000 + kOffsetNs), 0);
    clock.reset();
    CHECK(!clock.synced());
}

// Two lines with different local clocks select the same source frame at the
// same Console time.
void test_lines_agree_on_frame() {
    constexpr uint64_t period_ns = 16666666;
    SenderClock a;
    SenderClock b;
    for (int i = 0; i < 4; ++i) {
        const int64_t t = 1000000 + i * 250000;
        a.add(t, static_cast<uint64_t>(t * 1000 + 3000000000LL), static_cast<uint64_t>(t * 1000 + 3000000000LL), t);
        const int64_t u = 90000000 + i * 250000;
        b.add(u, static_cast<uint64_t>(u * 1000 - 84000000000LL), static_cast<uint64_t>(u * 1000 - 84000000000LL), u);
    }
    CHECK(a.synced() && b.synced());
    for (uint64_t console_ns = 6000000000ULL; console_ns < 6500000000ULL; console_ns += 1000000) {
        const int64_t local_a = (static_cast<int64_t>(console_ns) - 3000000000LL) / 1000;
        const int64_t local_b = (static_cast<int64_t>(console_ns) + 84000000000LL) / 1000;
        auto pick = [&](const SenderClock& clock, int64_t local) {
            const uint64_t due = static_cast<uint64_t>(clock.sender_ns(local));
            uint64_t selected = 0;
            for (uint64_t frame = 300; frame < 420; ++frame) {
                if (frame_due(frame * period_ns, due, 8333)) selected = frame;
            }
            return selected;
        };
        CHECK_EQ(pick(a, local_a), pick(b, local_b));
    }
}

}  // namespace

void test_sender_list() {
    SenderList senders;
    CHECK(!senders.allowed(0x0101a8c0u, 0));
    senders.allow(0x0101a8c0u, 100);
    CHECK(senders.allowed(0x0101a8c0u, 99));
    CHECK(!senders.allowed(0x0101a8c0u, 100));  // the login session expired
    CHECK(!senders.allowed(0x0201a8c0u, 50));
    CHECK(!senders.allowed(0, 50));
    senders.allow(0x0101a8c0u, 50);  // an older renewal never shortens access
    CHECK(senders.allowed(0x0101a8c0u, 99));
    for (uint32_t ip = 2; ip <= 5; ++ip) senders.allow(ip, 200 + ip);
    CHECK(!senders.allowed(0x0101a8c0u, 99));  // earliest expiry made room
    for (uint32_t ip = 2; ip <= 5; ++ip) CHECK(senders.allowed(ip, 150));
}

int main() {
    test_crc_golden();
    test_v6_header_golden_and_strictness();
    test_chunk_geometry_is_exact();
    test_reorder_duplicate_and_conflict();
    test_single_loss_parity_recovery();
    test_two_slots_and_dropped_frame_isolation();
    test_frame_decode();
    test_lz4_decodes_in_place();
    test_short_frame_parity_matches_its_length();
    test_repeat_frame();
    test_sender_list();
    test_frame_serial_wrap();
    test_delayed_frames_cannot_cross_committed_boundary();
    test_bounded_jitter_reservoir();
    test_playout_tolerates_timer_boundary_jitter();
    test_clock_packets();
    test_sender_clock_prefers_shortest_exchange();
    test_lines_agree_on_frame();
    return hosttest::report("Frame v6");
}
