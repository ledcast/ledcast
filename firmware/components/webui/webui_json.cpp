#include "webui_json.h"

#include <cstdio>
#include <cstring>

namespace ledcast::webui {

JsonSink::JsonSink(char* buffer, size_t capacity, FlushFn flush, void* ctx)
    : buffer_(buffer), capacity_(capacity), flush_(flush), ctx_(ctx) {}

esp_err_t JsonSink::flush_buffer() {
    if (len_ == 0) {
        return ESP_OK;
    }
    if (flush_ == nullptr) {
        return ESP_OK;
    }
    const esp_err_t err = flush_(ctx_, buffer_, len_);
    len_ = 0;
    if (err != ESP_OK && err_ == ESP_OK) {
        err_ = err;
    }
    return err;
}

void JsonSink::put(const char* data, size_t len) {
    if (err_ != ESP_OK) {
        return;
    }
    while (len > 0) {
        // Один байт всегда держим под завершающий ноль: буфер отдаётся наружу
        // как обычная C-строка.
        const size_t room = capacity_ - 1 - len_;
        if (room == 0) {
            if (flush_ == nullptr) {
                err_ = ESP_ERR_NO_MEM;
                return;
            }
            if (flush_buffer() != ESP_OK) {
                return;
            }
            continue;
        }
        const size_t take = len < room ? len : room;
        memcpy(buffer_ + len_, data, take);
        len_ += take;
        data += take;
        len -= take;
    }
    buffer_[len_] = '\0';
}

void JsonSink::put_char(char c) { put(&c, 1); }

void JsonSink::separator() {
    if (need_comma_) {
        put_char(',');
    }
    need_comma_ = true;
}

void JsonSink::put_escaped(const char* value) {
    put_char('"');
    if (value != nullptr) {
        for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p != 0; ++p) {
            switch (*p) {
                case '"': put("\\\"", 2); break;
                case '\\': put("\\\\", 2); break;
                case '\n': put("\\n", 2); break;
                case '\r': put("\\r", 2); break;
                case '\t': put("\\t", 2); break;
                default:
                    if (*p < 0x20) {
                        char esc[7];
                        snprintf(esc, sizeof(esc), "\\u%04x", *p);
                        put(esc, 6);
                    } else {
                        put(reinterpret_cast<const char*>(p), 1);
                    }
                    break;
            }
        }
    }
    put_char('"');
}

void JsonSink::put_key(const char* key) {
    separator();
    if (key != nullptr) {
        put_escaped(key);
        put_char(':');
    }
}

void JsonSink::object_open(const char* key) {
    put_key(key);
    put_char('{');
    need_comma_ = false;
}

void JsonSink::object_close() {
    put_char('}');
    need_comma_ = true;
}

void JsonSink::array_open(const char* key) {
    put_key(key);
    put_char('[');
    need_comma_ = false;
}

void JsonSink::array_close() {
    put_char(']');
    need_comma_ = true;
}

void JsonSink::string(const char* key, const char* value) {
    put_key(key);
    put_escaped(value);
}

void JsonSink::integer(const char* key, long long value) {
    put_key(key);
    char tmp[24];
    const int n = snprintf(tmp, sizeof(tmp), "%lld", value);
    put(tmp, static_cast<size_t>(n));
}

void JsonSink::real(const char* key, double value, int decimals) {
    put_key(key);
    char tmp[32];
    const int n = snprintf(tmp, sizeof(tmp), "%.*f", decimals, value);
    put(tmp, static_cast<size_t>(n));
}

void JsonSink::boolean(const char* key, bool value) {
    put_key(key);
    if (value) {
        put("true", 4);
    } else {
        put("false", 5);
    }
}

void JsonSink::null(const char* key) {
    put_key(key);
    put("null", 4);
}

esp_err_t JsonSink::finish() {
    if (err_ == ESP_OK && flush_ != nullptr) {
        flush_buffer();
    }
    return err_;
}

}  // namespace ledcast::webui
