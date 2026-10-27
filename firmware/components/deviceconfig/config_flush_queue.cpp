#include "config_flush_queue.h"

#include <cstring>

namespace ledcast {

void ConfigFlushQueue::push(std::unique_ptr<char[]> text, size_t length, const char* stored,
                            size_t stored_length) {
    if (!text || length == 0) {
        return;
    }
    const bool same_as_stored = stored != nullptr && stored_length == length &&
                                std::memcmp(stored, text.get(), length) == 0;
    if (same_as_stored) {
        // Отложенная запись, если она была, устарела: во флеше уже лежит то, что
        // просят сохранить. Пропустить себя и оставить её значило бы записать
        // позже старое значение поверх живого.
        text_.reset();
        length_ = 0;
        return;
    }
    text_ = std::move(text);
    length_ = length;
}

bool ConfigFlushQueue::take(std::unique_ptr<char[]>& text, size_t& length) {
    if (!text_) {
        return false;
    }
    text = std::move(text_);
    length = length_;
    length_ = 0;
    return true;
}

void ConfigFlushQueue::restore(std::unique_ptr<char[]> text, size_t length) {
    if (text_ || !text || length == 0) {
        return;
    }
    text_ = std::move(text);
    length_ = length;
}

}  // namespace ledcast
