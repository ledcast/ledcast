// Вход Console без пароля: вывод ключа, проверка proof, одноразовые nonce и таблица сессий.
// На устройстве HMAC даёт mbedtls; здесь — эталонный SHA-256, сверенный с RFC 4231.

#include <cstring>
#include <string>

#include "check.h"
#include "console_auth.h"

namespace {

using namespace ledcast::webui;

// --- эталонный SHA-256 (FIPS 180-4) только для теста --------------------------------

constexpr uint32_t kRound[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
    0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
    0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
    0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
    0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
    0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2};

uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

std::string sha256(const std::string& data) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::string msg = data;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) msg.push_back('\0');
    const uint64_t bits = static_cast<uint64_t>(data.size()) * 8;
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<char>(bits >> (i * 8)));
    for (size_t block = 0; block < msg.size(); block += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            const auto* p = reinterpret_cast<const uint8_t*>(msg.data() + block + i * 4);
            w[i] = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], k = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t t1 = k + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) +
                                ((e & f) ^ (~e & g)) + kRound[i] + w[i];
            const uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            k = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += k;
    }
    std::string out;
    for (uint32_t v : h) {
        for (int i = 3; i >= 0; --i) out.push_back(static_cast<char>(v >> (i * 8)));
    }
    return out;
}

bool reference_hmac(const uint8_t* key, size_t key_len, const uint8_t* message, size_t message_len,
                    uint8_t out[kAuthKeyBytes]) {
    std::string k(reinterpret_cast<const char*>(key), key_len);
    if (k.size() > 64) k = sha256(k);
    k.resize(64, '\0');
    std::string inner(64, '\0'), outer(64, '\0');
    for (int i = 0; i < 64; ++i) {
        inner[i] = static_cast<char>(k[i] ^ 0x36);
        outer[i] = static_cast<char>(k[i] ^ 0x5c);
    }
    const std::string mac =
        sha256(outer + sha256(inner + std::string(reinterpret_cast<const char*>(message), message_len)));
    memcpy(out, mac.data(), kAuthKeyBytes);
    return true;
}

std::string hex(const uint8_t* bytes, size_t n) {
    std::string out(n * 2, '\0');
    hex_encode(bytes, n, out.data());
    return out;
}

void random_bytes(uint8_t out[32], uint8_t seed) {
    for (int i = 0; i < 32; ++i) out[i] = static_cast<uint8_t>(seed * 31 + i);
}

void test_reference_hmac_matches_rfc4231() {
    const char key[] = "Jefe", data[] = "what do ya want for nothing?";
    uint8_t mac[kAuthKeyBytes];
    reference_hmac(reinterpret_cast<const uint8_t*>(key), 4, reinterpret_cast<const uint8_t*>(data),
                   strlen(data), mac);
    CHECK(hex(mac, sizeof(mac)) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

// Те же векторы проверяют Rust (desktop/src/pairing.rs) и мок строки (firmware/web/dev);
// посчитаны отдельно через Python hmac.
void test_console_key_known_answers() {
    uint8_t key[kAuthKeyBytes];
    CHECK(console_key(reference_hmac, "ledcast1", "ledcast-7f2a91", key));
    CHECK(hex(key, sizeof(key)) == "4e865ed217587b470a82e5c53c27188963cc2a4afd78afaac157a8935a544fb2");
    CHECK(console_key(reference_hmac, "88888888", "A0B1C2D3E4F5", key));
    CHECK(hex(key, sizeof(key)) == "35ca6fc7a96b71b89b46fa3d87f2bf81cc1e4e01db4e72df406118be54c887e4");
}

void test_proof() {
    uint8_t key[kAuthKeyBytes];
    console_key(reference_hmac, "ledcast1", "ledcast-7f2a91", key);
    const char nonce[] = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff";
    const char proof[] = "c8d68caa93494443820bbde7ecf42483760ec68b17ce934649cb6522c0dbbb16";
    CHECK(console_proof_ok(reference_hmac, key, nonce, proof));
    std::string wrong = proof;
    wrong[0] = 'd';
    CHECK(!console_proof_ok(reference_hmac, key, nonce, wrong.c_str()));
    CHECK(!console_proof_ok(reference_hmac, key, nonce, ""));
    uint8_t other[kAuthKeyBytes];
    console_key(reference_hmac, "ledcast2", "ledcast-7f2a91", other);
    CHECK(!console_proof_ok(reference_hmac, other, nonce, proof));
}

void test_nonce_is_single_use_and_expires() {
    NonceRing ring;
    uint8_t bytes[32];
    random_bytes(bytes, 1);
    const std::string first = ring.issue(bytes, 0);
    CHECK_EQ(first.size(), 64);
    CHECK(ring.consume(first.c_str(), 1000));
    CHECK(!ring.consume(first.c_str(), 1000));

    random_bytes(bytes, 2);
    const std::string stale = ring.issue(bytes, 0);
    CHECK(!ring.consume(stale.c_str(), NonceRing::kLifetimeUs));
    CHECK(!ring.consume(stale.c_str(), 0));  // просроченный тоже погашен

    CHECK(!ring.consume("", 0));
    CHECK(!ring.consume("zz", 0));
}

void test_nonce_ring_evicts_oldest() {
    NonceRing ring;
    uint8_t bytes[32];
    std::string issued[NonceRing::kSlots + 1];
    for (size_t i = 0; i <= NonceRing::kSlots; ++i) {
        random_bytes(bytes, static_cast<uint8_t>(10 + i));
        issued[i] = ring.issue(bytes, static_cast<int64_t>(i));
    }
    CHECK(!ring.consume(issued[0].c_str(), 10));
    for (size_t i = 1; i <= NonceRing::kSlots; ++i) CHECK(ring.consume(issued[i].c_str(), 10));
}

void test_sessions_coexist_and_evict_least_recent() {
    SessionTable sessions;
    uint8_t bytes[32];
    std::string tokens[SessionTable::kSlots];
    for (size_t i = 0; i < SessionTable::kSlots; ++i) {
        random_bytes(bytes, static_cast<uint8_t>(40 + i));
        tokens[i] = sessions.open(bytes, static_cast<int64_t>(i));
    }
    for (size_t i = 0; i < SessionTable::kSlots; ++i) CHECK(sessions.check(tokens[i].c_str(), 100));
    CHECK(sessions.check(tokens[0].c_str(), 200));  // первая — самая свежая по использованию

    random_bytes(bytes, 90);
    const std::string extra = sessions.open(bytes, 300);
    CHECK(sessions.check(extra.c_str(), 301));
    CHECK(sessions.check(tokens[0].c_str(), 301));
    CHECK(!sessions.check(tokens[1].c_str(), 301));
    CHECK(sessions.check(tokens[2].c_str(), 301));

    CHECK(!sessions.check(extra.c_str(), 300 + SessionTable::kLifetimeUs));
    CHECK(!sessions.check("", 0));
    CHECK(!sessions.check(std::string(64, '0').c_str(), 0));
}

}  // namespace

int main() {
    test_reference_hmac_matches_rfc4231();
    test_console_key_known_answers();
    test_proof();
    test_nonce_is_single_use_and_expires();
    test_nonce_ring_evicts_oldest();
    test_sessions_coexist_and_evict_least_recent();
    return hosttest::report("console_auth");
}
