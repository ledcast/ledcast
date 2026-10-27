#pragma once

#include <cstddef>
#include <cstdint>

#include <esp_err.h>

namespace ledcast::webui {

// Потоковая сборка JSON в буфер фиксированного размера.
//
// Свободной памяти на плате мало, а статус и список файлов легко перерастают
// килобайт, поэтому документ никогда не собирается целиком: как только буфер
// заполняется, содержимое уходит через flush (HTTP-чанк) и буфер переиспользуется.
// Без flush (WebSocket, где кадр обязан быть цельным) переполнение — ошибка.
class JsonSink {
 public:
    using FlushFn = esp_err_t (*)(void* ctx, const char* data, size_t len);

    JsonSink(char* buffer, size_t capacity, FlushFn flush, void* ctx);

    void object_open(const char* key = nullptr);
    void object_close();
    void array_open(const char* key = nullptr);
    void array_close();

    void string(const char* key, const char* value);
    void integer(const char* key, long long value);
    void real(const char* key, double value, int decimals);
    void boolean(const char* key, bool value);
    void null(const char* key);

    // Дописывает остаток. Для режима без flush после вызова доступны data()/length().
    esp_err_t finish();

    esp_err_t status() const { return err_; }
    const char* data() const { return buffer_; }
    size_t length() const { return len_; }

 private:
    void put(const char* data, size_t len);
    void put_char(char c);
    void put_key(const char* key);
    void put_escaped(const char* value);
    void separator();
    esp_err_t flush_buffer();

    char* buffer_;
    size_t capacity_;
    size_t len_ = 0;
    FlushFn flush_;
    void* ctx_;
    bool need_comma_ = false;
    esp_err_t err_ = ESP_OK;
};

}  // namespace ledcast::webui
