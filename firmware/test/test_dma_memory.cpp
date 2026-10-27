#include "check.h"
#include <cstdlib>
#include <initializer_list>
#include "deviceconfig.h"
#include "../components/hub75-panel/src/bcm_memory.h"
using namespace ledcast;

int main() {
    DeviceConfig cfg = config_defaults();
    CHECK_EQ(config_dma_bytes_per_module(cfg.line.module,8,2),8192);
    CHECK_EQ(config_dma_geometry(cfg.line.module).width,64);
    CHECK_EQ(config_dma_geometry(cfg.line.module).rows,4);
    for (uint8_t count=1; count<=kMaxLineModules; ++count) {
        cfg.line.module_count=count;
        for (uint8_t i=0;i<count;++i) cfg.line.modules[i].physical_module=i;
        for (uint8_t depth=2;depth<=8;++depth) for(uint8_t buffers=1;buffers<=2;++buffers) {
            cfg.display.bit_depth=depth; cfg.display.buffers=buffers;
            for (auto bus:kBusFrequencies) for (uint8_t rate:kFrameRates) {
                cfg.display.bus_hz=bus; cfg.display.frame_rate=rate;
                const auto g=config_dma_geometry(cfg.line.module);
                const uint32_t width=g.width*count;
                const auto memory=hub75_bcm::dma_memory(width,g.rows,depth,buffers,bus,rate,false);
                // Simulate exact pool allocations: each aligned row payload,
                // the shared scan pad, plus every split span's and pad
                // chunk's 12-byte descriptor in each ring.
                uint32_t bytes=0, descriptors=0, clocks=0;
                for(uint8_t row=0;row<g.rows;++row)
                    bytes+=hub75_bcm::align4(width*depth*2);
                hub75_bcm::for_each_span(g.rows,depth,memory.transition,false,
                    [&](uint8_t,uint8_t,uint8_t planes) {
                        uint32_t remaining=width*2u*planes;
                        clocks+=width*planes;
                        while(remaining) { const auto chunk=remaining>4092?4092:remaining; remaining-=chunk; ++descriptors; }
                    });
                uint32_t pad=memory.pad_clocks*2u;
                while(pad) { const auto chunk=pad>4092?4092:pad; pad-=chunk; ++descriptors; }
                bytes=(bytes+hub75_bcm::align4(descriptors*12))*buffers+memory.pad_buffer_bytes;
                CHECK_EQ(config_dma_bytes(cfg.line,cfg.display,depth),bytes);
                CHECK_EQ(memory.descriptors_per_buffer,descriptors);
                CHECK_EQ(memory.pass_clocks,clocks+memory.pad_clocks);
                CHECK_EQ(memory.pad_clocks%2u,0u);
                CHECK_EQ(config_validate(cfg,nullptr,0)==ESP_OK,bytes<=kDmaPoolBytes);
                // Кадр встаёт ровно на каждый passes_per_frame-й проход: сетка
                // кадров совпадает с частотой кадров с точностью до округления
                // прохода до чётного числа тактов.
                if (memory.pad_clocks!=0) {
                    const int64_t frame=int64_t(memory.pass_clocks)*memory.passes_per_frame*rate;
                    CHECK(std::llabs(frame-int64_t(bus))<=int64_t(2)*memory.passes_per_frame*rate);
                }
                // Проход не короче своих данных и не длиннее кадра, пока
                // развёртка вообще успевает за частотой кадров.
                if (uint64_t(clocks)*rate<=bus) CHECK(uint64_t(memory.pass_clocks)*rate<=bus+2u*rate);
                // Развёртка не реже 120 Гц, пока проход вообще в неё укладывается.
                if (uint64_t(clocks)*hub75_bcm::kMinRefreshHz<=bus && memory.pad_clocks*hub75_bcm::kMaxPadShare<=memory.pass_clocks)
                    CHECK(memory.refresh_hz>=hub75_bcm::kMinRefreshHz-1u);
            }
        }
    }
    // Строка пользователя: пять P10 32x16, 8 бит, две копии, 16 МГц, 120 кадров.
    // Развёртка 360 Гц, кадр на каждый третий проход, добивка 2% прохода.
    {
        const auto m=hub75_bcm::dma_memory(320,4,8,2,16000000,120,false);
        CHECK_EQ(m.transition,3);
        CHECK_EQ(m.passes_per_frame,3);
        CHECK_EQ(m.refresh_hz,360u);
        CHECK(m.pad_clocks*hub75_bcm::kMaxPadShare<=m.pass_clocks);
    }
    cfg=config_defaults(); cfg.line.module_count=5;
    for(uint8_t i=0;i<5;++i) cfg.line.modules[i].physical_module=i;
    CHECK(config_validate(cfg,nullptr,0)==ESP_OK);
    CHECK(config_dma_bytes(cfg.line,cfg.display,8)<48*1024);
    const auto capacity=config_chain_capacity(cfg.line,cfg.display,8,2);
    cfg.line.module_count=18;
    CHECK(config_dma_bytes(cfg.line,cfg.display,8)>kDmaPoolBytes);
    std::printf("Spiral32x16: five=%u bytes; 18=%u bytes; max8bitdouble=%u\n",
        [&] { auto five=cfg.line;five.module_count=5;return config_dma_bytes(five,cfg.display,8); }(),
        config_dma_bytes(cfg.line,cfg.display,8),capacity);
    CHECK(capacity>=10);
    // Stretching changes geometry, not always total pixel allocation.
    cfg.line.module.scan_layout=ScanLayout::P10Quarter;
    CHECK_EQ(config_dma_bytes_per_module(cfg.line.module,8,2),16384);
    cfg.line.module={}; cfg.line.module.width=64;cfg.line.module.height=64;
    cfg.line.module.scan_layout=ScanLayout::FourScan64;
    CHECK_EQ(config_dma_bytes_per_module(cfg.line.module,8,2),65536);
    CHECK_EQ(config_dma_geometry(cfg.line.module).rows,16);
    cfg.line.module.scan_layout=ScanLayout::TwoScan;
    CHECK_EQ(config_dma_bytes_per_module(cfg.line.module,8,2),65536);
    CHECK_EQ(config_dma_geometry(cfg.line.module).rows,32);
    return hosttest::failures?1:0;
}
