#include "config_snapshot.h"

#include <atomic>
#include <new>
#include <utility>

namespace ledcast {

struct ConfigSnapshot::Node {
    std::atomic<unsigned> references{1};
    const DeviceConfig config;
    explicit Node(const DeviceConfig& value) : config(value) {}
};

ConfigSnapshot config_snapshot_create(const DeviceConfig& cfg) {
    return ConfigSnapshot(new (std::nothrow) ConfigSnapshot::Node(cfg));
}

ConfigSnapshot config_snapshot_defaults() {
    // The initial reference belongs permanently to static storage, so this
    // revision is available even when the heap cannot allocate a snapshot.
    static ConfigSnapshot::Node defaults(config_defaults());
    defaults.references.fetch_add(1, std::memory_order_relaxed);
    return ConfigSnapshot(&defaults);
}

ConfigSnapshot::ConfigSnapshot(const ConfigSnapshot& other) : node_(other.node_) {
    if (node_) node_->references.fetch_add(1, std::memory_order_relaxed);
}

ConfigSnapshot::ConfigSnapshot(ConfigSnapshot&& other) noexcept : node_(other.node_) {
    other.node_ = nullptr;
}

ConfigSnapshot& ConfigSnapshot::operator=(ConfigSnapshot other) noexcept {
    swap(other);
    return *this;
}

ConfigSnapshot::~ConfigSnapshot() {
    if (node_ && node_->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete node_;
    }
}

void ConfigSnapshot::swap(ConfigSnapshot& other) noexcept {
    std::swap(node_, other.node_);
}

const DeviceConfig& ConfigSnapshot::operator*() const { return node_->config; }
const DeviceConfig* ConfigSnapshot::operator->() const { return &node_->config; }

}  // namespace ledcast
