#pragma once

#include <cstdint>

// Счёт времени вокруг обмена копиями кадра.
//
// Обмен переставляет ссылку в последнем дескрипторе кольца и вступает в силу
// только тогда, когда DMA до этого дескриптора дойдёт. Ждать этого события
// приходится по извещению от DMA, а у всякого ожидания должен быть предел.
// Предел считается из того же, из чего складывается длительность прохода
// кольца: сколько байт в кольце и с какой скоростью шина их вычитывает.
//
// Здесь только арифметика — ни ESP-IDF, ни FreeRTOS, — поэтому она проверяется
// обычным тестом на хосте.

namespace hub75_flip {

// Ниже этого предела ждать нечего: на коротком кольце проход занимает единицы
// микросекунд, и весь бюджет уходит на задержку прерывания и планировщика.
inline constexpr uint32_t kMinWaitTimeoutUs = 5000u;

// Выше этого предела ожидание перестаёт быть ожиданием и становится зависанием.
inline constexpr uint32_t kMaxWaitTimeoutUs = 500000u;

// Проход кольца целиком, микросекунды. Шина LCD_CAM настроена на 16 бит и
// забирает два байта за такт, поэтому байты делятся пополам.
inline uint32_t ring_period_us(uint32_t ring_bytes, uint32_t pclk_hz) {
    if (ring_bytes == 0u || pclk_hz == 0u) {
        return 0u;
    }
    const uint64_t words = static_cast<uint64_t>(ring_bytes) / 2u;
    const uint64_t us = (words * 1000000ull) / static_cast<uint64_t>(pclk_hz);
    return us > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(us);
}

// Предел ожидания извещения об обмене.
//
// Запись ссылки может не попасть в цель: если DMA как раз обрабатывает
// последний дескриптор, он уже прочитал старое значение и уйдёт на второй круг.
// Значит обмен вступает в силу не позже конца второго прохода. Третий проход —
// запас на задержку прерывания и на то, что ждущая задача не самая срочная.
inline uint32_t wait_timeout_us(uint32_t ring_bytes, uint32_t pclk_hz) {
    const uint64_t budget = 3ull * ring_period_us(ring_bytes, pclk_hz);
    if (budget < kMinWaitTimeoutUs) {
        return kMinWaitTimeoutUs;
    }
    if (budget > kMaxWaitTimeoutUs) {
        return kMaxWaitTimeoutUs;
    }
    return static_cast<uint32_t>(budget);
}

// Дескриптор по этому адресу принадлежит цепочке?
//
// Вопрос «обмен уже вступил в силу» решается именно так: у DMA спрашивается
// адрес дескриптора, который он обрабатывает сейчас, и проверяется, в какой из
// двух цепочек этот адрес лежит.
inline bool addr_in_chain(uintptr_t addr, uintptr_t chain_base, uint32_t count,
                          uint32_t descriptor_size) {
    if (addr < chain_base) {
        return false;
    }
    return (addr - chain_base) < static_cast<uintptr_t>(count) * descriptor_size;
}

}  // namespace hub75_flip
