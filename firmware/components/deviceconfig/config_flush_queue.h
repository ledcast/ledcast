#pragma once

#include <cstddef>
#include <memory>

// Очередь отложенной записи конфигурации во флеш.
//
// Правки прилетают чаще, чем их стоит писать: подбор яркости ползунком даёт
// десятки сохранений в минуту, а ресурс флеша конечен. Поэтому у каждого домена
// настроек своя очередь: она держит ровно один текст — последний — и
// отбрасывает запись, если во флеше уже лежит то же самое.
//
// Синхронизацию очередь на себя не берёт: её зовут из веб-задачи и из задачи
// таймера, замок держит хранилище.

namespace ledcast {

class ConfigFlushQueue {
public:
    // Правка. stored — то, что лежит во флеше сейчас, или nullptr, если прочитать
    // его не удалось.
    void push(std::unique_ptr<char[]> text, size_t length, const char* stored,
              size_t stored_length);

    // Забрать текст под запись. Очередь остаётся пустой.
    bool take(std::unique_ptr<char[]>& text, size_t& length);

    // Вернуть текст, который не удалось записать. Правка, прилетевшая за время
    // записи, свежее — её не затираем.
    void restore(std::unique_ptr<char[]> text, size_t length);

    bool empty() const { return text_ == nullptr; }

private:
    std::unique_ptr<char[]> text_;
    size_t length_ = 0;
};

}  // namespace ledcast
