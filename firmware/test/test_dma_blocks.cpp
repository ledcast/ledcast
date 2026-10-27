#include "check.h"
#include "dmapool.h"
#include <esp_heap_caps.h>
#include "../components/dmapool/include/owned_dma_blocks.h"
#include "../components/hub75-panel/src/bcm_memory.h"
#include <cstdlib>
#include <limits>
#include <unordered_set>

namespace {
std::unordered_set<void*> live;
size_t largest = 12288;
int calls = 0, fail_at = -1;
void* allocate(size_t bytes) {
    const int call = calls++;
    if (call == fail_at || bytes > largest) return nullptr;
    void* out = std::malloc(bytes);
    if (out) live.insert(out);
    return out;
}
void release(void* ptr) {
    CHECK_EQ(live.erase(ptr),1u);
    std::free(ptr);
}
size_t expected_blocks = 0;
bool configure(ledcast::OwnedDmaBlocks& pool, uint8_t count) {
    const auto plan=hub75_bcm::dma_memory(64*count,4,8,2,20000000,120,false);
    for(unsigned i=0;i<8;++i) if(!pool.take(64u*count*8*2,allocate)) return false;
    expected_blocks=10u+(plan.pad_buffer_bytes?1u:0u);
    if(plan.pad_buffer_bytes && !pool.take(plan.pad_buffer_bytes,allocate)) return false;
    for(unsigned i=0;i<2;++i) if(!pool.take(plan.descriptors_per_buffer*12,allocate)) return false;
    CHECK_EQ(pool.used(),plan.total_bytes);
    return true;
}
}
void* heap_caps_malloc(size_t bytes,uint32_t caps) {
    CHECK_EQ(caps,MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA);
    return allocate(bytes);
}
void heap_caps_free(void* ptr) { release(ptr); }
size_t heap_caps_get_largest_free_block(uint32_t) { return largest; }
int main() {
    ledcast::OwnedDmaBlocks pool(92*1024);
    // No free contiguous 46 KiB block exists, but all individual DMA payloads
    // fit. Growing/shrinking does not retain unused old capacity.
    for(unsigned cycle=0;cycle<1000;++cycle) {
        CHECK(configure(pool,cycle%5+1));
        CHECK_EQ(live.size(),expected_blocks);
        pool.clear(release);
        CHECK_EQ(pool.used(),0u);
        CHECK(live.empty());
    }
    // Inject failure at every row/descriptor allocation. Destruction cleans
    // precisely the owned prefix, and a following rollback can start again.
    for(int failure=0;failure<10;++failure) {
        calls=0;fail_at=failure;
        CHECK(!configure(pool,5));
        CHECK_EQ(live.size(),failure);
        pool.clear(release);
        CHECK(live.empty());
        fail_at=-1;
        CHECK(configure(pool,1));
        pool.clear(release);
    }
    CHECK(pool.take(0,allocate)==nullptr);
    CHECK(pool.take(std::numeric_limits<size_t>::max(),allocate)==nullptr);
    CHECK(pool.take(92*1024+1,allocate)==nullptr);
    for(size_t i=0;i<ledcast::OwnedDmaBlocks::kMaxBlocks;++i) CHECK(pool.take(1,allocate)!=nullptr);
    CHECK_EQ(pool.used(),ledcast::OwnedDmaBlocks::kMaxBlocks*4);
    CHECK(pool.take(4,allocate)==nullptr);
    pool.clear(release);
    pool.clear(release); // idempotent; no double free
    CHECK(live.empty());
    // Compile and exercise the actual ESP-facing owner against the injected
    // heap, not only its pure ledger implementation.
    CHECK(!ledcast::dma_pool_ready());
    CHECK(ledcast::dma_pool_allocate(4)==nullptr);
    CHECK_EQ(ledcast::dma_pool_init(),ESP_OK);
    CHECK_EQ(ledcast::dma_pool_init(),ESP_ERR_INVALID_STATE);
    CHECK(ledcast::dma_pool_ready());
    CHECK_EQ(ledcast::dma_pool_capacity(),92u*1024);
    CHECK_EQ(ledcast::dma_pool_used(),0u);
    CHECK(live.empty()); // initialization does not reserve 92 KiB
    CHECK(ledcast::dma_pool_allocate(513)!=nullptr);
    CHECK_EQ(ledcast::dma_pool_used(),516u);
    CHECK(ledcast::dma_pool_allocate(largest+1)==nullptr);
    ledcast::dma_pool_reset();
    CHECK_EQ(ledcast::dma_pool_used(),0u);
    CHECK(live.empty());
    return hosttest::failures?1:0;
}
