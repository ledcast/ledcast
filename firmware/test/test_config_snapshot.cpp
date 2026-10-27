#include "check.h"
#include "config_snapshot.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <thread>
#include <utility>

int main() {
    using namespace ledcast;
    const auto defaults = config_snapshot_defaults();
    CHECK(static_cast<bool>(defaults));
    {
        auto another = config_snapshot_defaults();
        CHECK(another == defaults);
    }
    CHECK(config_snapshot_defaults() == defaults);
    auto config = config_defaults();
    std::strcpy(config.show.text, "original");
    auto published = config_snapshot_create(config);
    CHECK(static_cast<bool>(published));
    auto reader = published;
    CHECK(reader == published);
    const DeviceConfig* original = &*reader;

    // Replacing more than two revisions previously overwrote a live reader.
    for (unsigned i = 0; i < 32; ++i) {
        config.display.brightness = static_cast<uint8_t>(i);
        std::snprintf(config.show.text, sizeof(config.show.text), "revision %u", i);
        auto next = config_snapshot_create(config);
        CHECK(static_cast<bool>(next));
        published.swap(next);
        CHECK(!(reader == published));
        CHECK_EQ(published->display.brightness, i);
        CHECK(&*reader == original);
        CHECK(std::strcmp(reader->show.text, "original") == 0);
    }

    // A reader on another task may copy and release its revision concurrently
    // with other readers, including after the publisher releases its owner.
    std::atomic<bool> intact{true};
    auto read_many = [&intact](ConfigSnapshot held) {
        for (unsigned i = 0; i < 10000; ++i) {
            auto copy = held;
            auto moved = std::move(copy);
            if (copy || std::strcmp(moved->show.text, "original") != 0) {
                intact.store(false);
            }
        }
    };
    std::thread first(read_many, reader);
    std::thread second(read_many, reader);
    reader = {};
    published = {};
    first.join();
    second.join();
    CHECK(intact.load());
    CHECK(!reader);
    CHECK(!published);
    return hosttest::report("config_snapshot");
}
