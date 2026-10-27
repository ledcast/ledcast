#pragma once
#include <cstddef>
#include <esp_err.h>

namespace ledcast {
// Single owner for row payloads and descriptor rings. Capacity is a hard
// budget, not a permanent allocation. Driver setup runs before Wi-Fi at boot;
// live rebuilds are protected by the display generation lock.
esp_err_t dma_pool_init();
bool dma_pool_ready();
size_t dma_pool_capacity();
size_t dma_pool_used();
// Never from an ISR or while a DMA channel still references these blocks.
// Driver destruction stops/deletes its channel before resetting ownership.
void dma_pool_reset();
void* dma_pool_allocate(size_t bytes);
} // namespace ledcast
