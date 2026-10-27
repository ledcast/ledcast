#include <cstring>
#include <memory>

#include "check.h"
#include "config_flush_queue.h"

using ledcast::ConfigFlushQueue;

namespace {

std::unique_ptr<char[]> copy_of(const char* text, size_t& length) {
    length = std::strlen(text);
    std::unique_ptr<char[]> out(new char[length + 1]);
    std::memcpy(out.get(), text, length + 1);
    return out;
}

void push(ConfigFlushQueue& queue, const char* text, const char* stored) {
    size_t length = 0;
    std::unique_ptr<char[]> copy = copy_of(text, length);
    queue.push(std::move(copy), length, stored, stored != nullptr ? std::strlen(stored) : 0);
}

// Что очередь отдаст под запись: пустая строка, если писать нечего.
const char* taken(ConfigFlushQueue& queue, std::unique_ptr<char[]>& hold) {
    size_t length = 0;
    if (!queue.take(hold, length)) {
        return "";
    }
    return hold.get();
}

void test_last_edit_wins() {
    ConfigFlushQueue queue;
    std::unique_ptr<char[]> hold;
    CHECK(queue.empty());

    push(queue, "B", "A");
    push(queue, "C", "A");
    CHECK(std::strcmp(taken(queue, hold), "C") == 0);
    CHECK(queue.empty());
}

void test_skips_write_of_stored_text() {
    ConfigFlushQueue queue;
    std::unique_ptr<char[]> hold;

    push(queue, "A", "A");
    CHECK(queue.empty());
    CHECK(std::strcmp(taken(queue, hold), "") == 0);
}

// Правка, вернувшая настройки к тому, что лежит во флеше, обязана отменить
// отложенную запись, а не пропустить себя. Иначе во флеш уедет предыдущее
// значение, а живая конфигурация останется другой — и перезагрузка поднимет
// прибор со старой настройкой.
void test_return_to_stored_cancels_pending() {
    ConfigFlushQueue queue;
    std::unique_ptr<char[]> hold;

    push(queue, "B", "A");
    push(queue, "A", "A");
    CHECK(queue.empty());
    CHECK(std::strcmp(taken(queue, hold), "") == 0);
}

void test_failed_write_returns_text() {
    ConfigFlushQueue queue;
    std::unique_ptr<char[]> hold;

    push(queue, "B", "A");
    size_t length = 0;
    CHECK(queue.take(hold, length));
    queue.restore(std::move(hold), length);
    CHECK(!queue.empty());
    CHECK(std::strcmp(taken(queue, hold), "B") == 0);
}

void test_failed_write_does_not_overwrite_newer_edit() {
    ConfigFlushQueue queue;
    std::unique_ptr<char[]> hold;

    push(queue, "B", "A");
    size_t length = 0;
    CHECK(queue.take(hold, length));
    push(queue, "C", "A");
    queue.restore(std::move(hold), length);
    CHECK(std::strcmp(taken(queue, hold), "C") == 0);
}

void test_no_stored_blob_queues_everything() {
    ConfigFlushQueue queue;
    std::unique_ptr<char[]> hold;

    push(queue, "A", nullptr);
    CHECK(std::strcmp(taken(queue, hold), "A") == 0);
}

}  // namespace

int main() {
    test_last_edit_wins();
    test_skips_write_of_stored_text();
    test_return_to_stored_cancels_pending();
    test_failed_write_returns_text();
    test_failed_write_does_not_overwrite_newer_edit();
    test_no_stored_blob_queues_everything();
    return hosttest::report("config_flush");
}
