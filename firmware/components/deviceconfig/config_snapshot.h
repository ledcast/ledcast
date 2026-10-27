#pragma once

#include "deviceconfig.h"

namespace ledcast {
// Allocate before applying hardware changes. Empty means allocation failed.
ConfigSnapshot config_snapshot_create(const DeviceConfig& cfg);
ConfigSnapshot config_snapshot_defaults();
}  // namespace ledcast
