#pragma once
inline void dma_test_log(const char*,const char*,...) {}
#define ESP_LOGE(...) dma_test_log(__VA_ARGS__)
