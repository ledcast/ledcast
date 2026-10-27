#pragma once

#include <cstddef>
#include <cstdint>

// Вход без пароля для LEDCAST Console и таблица сессий. Здесь только чистая логика:
// время, случайные байты и HMAC приходят снаружи, поэтому модуль собирается и на хосте.
//
// Ключ Console: K = HMAC-SHA256(пароль строки, "ledcast-console:" + id строки).
// Строка его не хранит, а вычисляет при каждом входе, поэтому смена пароля сразу
// отзывает доступ у всех Console. Ключ по сети не ходит: Console берёт одноразовый
// nonce и отвечает proof = hex(HMAC-SHA256(K, nonce)), где nonce — те же 64 hex-символа
// ASCII, что пришли от строки.

namespace ledcast::webui {

inline constexpr size_t kAuthKeyBytes = 32;
// 32 случайных байта в hex: и nonce, и токен сессии.
inline constexpr size_t kAuthHexChars = 64;
inline constexpr char kConsoleKeyLabel[] = "ledcast-console:";

// HMAC-SHA256; false — ошибка библиотеки.
using HmacSha256 = bool (*)(const uint8_t* key, size_t key_len, const uint8_t* message,
                            size_t message_len, uint8_t out[kAuthKeyBytes]);

// Сравнение строк за время, не зависящее от места первого расхождения.
bool secure_equal(const char* a, const char* b);

void hex_encode(const uint8_t* bytes, size_t count, char* out);  // out: 2 * count + 1

bool console_key(HmacSha256 hmac, const char* password, const char* device_id,
                 uint8_t out[kAuthKeyBytes]);

// proof — hex в нижнем регистре, как его считает Console.
bool console_proof_ok(HmacSha256 hmac, const uint8_t key[kAuthKeyBytes], const char* nonce,
                      const char* proof);

// Неотвеченные nonce: несколько штук, каждый живёт 30 с и годится на одну попытку.
class NonceRing {
public:
    static constexpr size_t kSlots = 4;
    static constexpr int64_t kLifetimeUs = 30LL * 1000000;
    static constexpr int kLifetimeSec = 30;

    // Занимает просроченную или самую старую ячейку; возвращает выданный nonce.
    const char* issue(const uint8_t random[kAuthHexChars / 2], int64_t now_us);
    // Гасит nonce в любом случае; true — он был выдан и ещё не истёк.
    bool consume(const char* nonce, int64_t now_us);

private:
    struct Slot {
        char nonce[kAuthHexChars + 1]{};
        int64_t expiry = 0;
    };
    Slot slots_[kSlots];
};

// Сессии браузера и Console: вход одного не выбивает другого. Когда ячейки заняты,
// новая сессия вытесняет ту, что дольше всех не использовалась.
class SessionTable {
public:
    static constexpr size_t kSlots = 4;
    static constexpr int64_t kLifetimeUs = 12LL * 60 * 60 * 1000000;
    static constexpr int kLifetimeSec = 12 * 60 * 60;

    const char* open(const uint8_t random[kAuthHexChars / 2], int64_t now_us);
    bool check(const char* token, int64_t now_us);

private:
    struct Slot {
        char token[kAuthHexChars + 1]{};
        int64_t expiry = 0;
        int64_t used = 0;
    };
    Slot slots_[kSlots];
};

}  // namespace ledcast::webui
