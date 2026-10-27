#pragma once
#include <cstddef>
#include <cstdint>
inline constexpr uint32_t MALLOC_CAP_INTERNAL=1;
inline constexpr uint32_t MALLOC_CAP_DMA=2;
void* heap_caps_malloc(size_t bytes,uint32_t caps);
void heap_caps_free(void* ptr);
size_t heap_caps_get_largest_free_block(uint32_t caps);
