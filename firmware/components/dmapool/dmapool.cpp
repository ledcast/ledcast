#include "dmapool.h"
#include "owned_dma_blocks.h"
#include <esp_heap_caps.h>
#include <esp_log.h>
#include "deviceconfig.h"

namespace ledcast {
namespace {
constexpr char kTag[] = "dmapool";
bool g_ready = false;
OwnedDmaBlocks g_blocks{kDmaPoolBytes};
void* allocate_dma(size_t bytes) {
    return heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
}
} // namespace

esp_err_t dma_pool_init() {
    if (g_ready) return ESP_ERR_INVALID_STATE;
    // Driver setup allocates payloads before Wi-Fi at boot. Permanently
    // reserving 92 KiB stranded 47–63 KiB on short lines while RGB admission
    // failed for lack of memory in the remaining general-purpose heap.
    g_ready = true;
    return ESP_OK;
}
bool dma_pool_ready() { return g_ready; }
size_t dma_pool_capacity() { return g_ready ? g_blocks.limit() : 0; }
size_t dma_pool_used() { return g_blocks.used(); }
void dma_pool_reset() { g_blocks.clear(heap_caps_free); }
void* dma_pool_allocate(size_t bytes) {
    if (!g_ready) return nullptr;
    void* block = g_blocks.take(bytes, allocate_dma);
    if (!block) {
        ESP_LOGE(kTag, "DMA allocation refused: request=%u used=%u limit=%u largest=%u",
                 unsigned(bytes), unsigned(g_blocks.used()), unsigned(g_blocks.limit()),
                 unsigned(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA)));
    }
    return block;
}
} // namespace ledcast
