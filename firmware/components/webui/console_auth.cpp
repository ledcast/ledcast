#include "console_auth.h"

#include <cstring>

namespace ledcast::webui {

bool secure_equal(const char* a, const char* b) {
    const size_t an = strlen(a), bn = strlen(b);
    unsigned diff = an ^ bn;
    const size_t n = an > bn ? an : bn;
    for (size_t i = 0; i < n; ++i) {
        diff |= static_cast<unsigned char>(i < an ? a[i] : 0) ^
                static_cast<unsigned char>(i < bn ? b[i] : 0);
    }
    return diff == 0;
}

void hex_encode(const uint8_t* bytes, size_t count, char* out) {
    static constexpr char kDigits[] = "0123456789abcdef";
    for (size_t i = 0; i < count; ++i) {
        out[i * 2] = kDigits[bytes[i] >> 4];
        out[i * 2 + 1] = kDigits[bytes[i] & 0x0f];
    }
    out[count * 2] = '\0';
}

bool console_key(HmacSha256 hmac, const char* password, const char* device_id,
                 uint8_t out[kAuthKeyBytes]) {
    // Метка плюс id строки (12 hex-символов MAC); запас на длинный id.
    char message[sizeof(kConsoleKeyLabel) + 64];
    const size_t label = sizeof(kConsoleKeyLabel) - 1, id = strlen(device_id);
    if (id >= sizeof(message) - label) return false;
    memcpy(message, kConsoleKeyLabel, label);
    memcpy(message + label, device_id, id + 1);
    return hmac(reinterpret_cast<const uint8_t*>(password), strlen(password),
                reinterpret_cast<const uint8_t*>(message), label + id, out);
}

bool console_proof_ok(HmacSha256 hmac, const uint8_t key[kAuthKeyBytes], const char* nonce,
                      const char* proof) {
    uint8_t expected[kAuthKeyBytes];
    if (!hmac(key, kAuthKeyBytes, reinterpret_cast<const uint8_t*>(nonce), strlen(nonce),
              expected)) {
        return false;
    }
    char hex[kAuthHexChars + 1];
    hex_encode(expected, sizeof(expected), hex);
    return secure_equal(hex, proof);
}

const char* NonceRing::issue(const uint8_t random[kAuthHexChars / 2], int64_t now_us) {
    Slot* target = &slots_[0];
    for (auto& slot : slots_) {
        if (slot.expiry <= now_us) {
            target = &slot;
            break;
        }
        if (slot.expiry < target->expiry) target = &slot;
    }
    hex_encode(random, kAuthHexChars / 2, target->nonce);
    target->expiry = now_us + kLifetimeUs;
    return target->nonce;
}

bool NonceRing::consume(const char* nonce, int64_t now_us) {
    if (strlen(nonce) != kAuthHexChars) return false;
    bool valid = false;
    for (auto& slot : slots_) {
        if (!slot.nonce[0] || !secure_equal(slot.nonce, nonce)) continue;
        valid = slot.expiry > now_us;
        slot = Slot{};
    }
    return valid;
}

const char* SessionTable::open(const uint8_t random[kAuthHexChars / 2], int64_t now_us) {
    Slot* target = &slots_[0];
    for (auto& slot : slots_) {
        if (!slot.token[0] || slot.expiry <= now_us) {
            target = &slot;
            break;
        }
        if (slot.used < target->used) target = &slot;
    }
    hex_encode(random, kAuthHexChars / 2, target->token);
    target->expiry = now_us + kLifetimeUs;
    target->used = now_us;
    return target->token;
}

bool SessionTable::check(const char* token, int64_t now_us) {
    if (strlen(token) != kAuthHexChars) return false;
    bool found = false;
    for (auto& slot : slots_) {
        if (!slot.token[0] || slot.expiry <= now_us || !secure_equal(slot.token, token)) continue;
        slot.used = now_us;
        found = true;
    }
    return found;
}

}  // namespace ledcast::webui
