#include "ESP32-HUB75-MatrixPanel-I2S-DMA.h"

#include "dmapool.h"
#include "bcm_timing.h"
#include "bcm_memory.h"

#include <algorithm>

MatrixPanel_I2S_DMA::~MatrixPanel_I2S_DMA() {
  // Unregister GDMA/its ISR before freeing any descriptor or pixel payload.
  // This also releases allocations after a partially successful begin().
  dma_bus.release();
  ledcast::dma_pool_reset();
}

#if defined(SPIRAM_DMA_BUFFER)
// Sprite_TM saves the day again...
// https://www.esp32.com/viewtopic.php?f=2&t=30584
#include "rom/cache.h"
#endif

/* This replicates same function in rowBitStruct, but due to induced inlining it might be MUCH faster
 * when used in tight loops while method from struct could be flushed out of instruction cache between
 * loop cycles do NOT forget about buff_id param if using this.
 */
// #define getRowDataPtr(row, _dpth, buff_id) &(dma_buff.rowBits[row]->data[_dpth * dma_buff.rowBits[row]->width + buff_id*(dma_buff.rowBits[row]->width * dma_buff.rowBits[row]->colour_depth)])

// BufferID is now ignored, seperate global pointer pointer!
#define getRowDataPtr(row, _dpth) &(fb->rowBits[row]->data[_dpth * fb->rowBits[row]->width])




  /* Linear scaling with rounding: 8-bit input onto the exact range the bitplane loop
   * below reads back. The colour depth is a runtime setting, so the scale is computed
   * from it at runtime too. */
  #define SCALE_TO_COLOUR_DEPTH()                                                                    \
    uint16_t red16   = red * 256u;                                                                   \
    uint16_t green16 = green * 256u;                                                                 \
    uint16_t blue16  = blue * 256u;                                                                  \
                                                                                                     \
    uint8_t shift_amount = 16 - m_cfg.getPixelColorDepthBits();                                      \
    uint16_t rounding = (1 << (shift_amount - 1))-1;                                                 \
    uint16_t max_val = (1 << m_cfg.getPixelColorDepthBits()) -1;                                     \
    uint16_t red_val   = (red16 + rounding) >> shift_amount;                                         \
    uint16_t green_val = (green16 + rounding) >> shift_amount;                                       \
    uint16_t blue_val  = (blue16 + rounding) >> shift_amount;                                        \
    red_val = red_val > max_val ? max_val : red_val;                                                 \
    green_val = green_val > max_val ? max_val : green_val;                                           \
    blue_val = blue_val > max_val ? max_val : blue_val;




/* This library is designed to take an 8 bit / 1 byte value (0-255) for each R G B colour sub-pixel.
 * Input is scaled to 16-bit range, then shifted down to the target bit depth with rounding.
 */

bool MatrixPanel_I2S_DMA::setupDMA(const HUB75_I2S_CFG &_cfg)
{
  
  /***
   * Step 0: Allocate basic DMA framebuffer memory for the data we send out in parallel to the HUB75 panel.
   *         Colour depth is the only consideration.
   * 
   */
  ESP_LOGI("I2S-DMA", "Free heap: %d", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  ESP_LOGI("I2S-DMA", "Free SPIRAM: %d", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

  // Раскладка строится заново, прежняя больше никому не нужна: сюда приходят
  // только из begin(), а панель к этому моменту либо ещё не поднималась, либо
  // уже удалена вместе с остановленным DMA.
  ledcast::dma_pool_reset();

  const size_t row_bytes =
      static_cast<size_t>(PIXELS_PER_ROW) * m_cfg.getPixelColorDepthBits() * sizeof(ESP32_I2S_DMA_STORAGE_TYPE);

  size_t allocated_fb_memory = 0;

  int fbs_required = (m_cfg.double_buff) ? 2 : 1;

  for (int fb = 0; fb < (fbs_required); fb++)
  {
    if (ROWS_PER_FRAME > frame_buffer[fb].rowBits.size()) return false;
    frame_buffer[fb].rows = 0;

    for (int malloc_num = 0; malloc_num < ROWS_PER_FRAME; malloc_num++)
    {
      // Each row is contiguous, but independent rows need not be adjacent.
      // The owner tracks all blocks and releases them after DMA is stopped.
      auto *row_data = static_cast<ESP32_I2S_DMA_STORAGE_TYPE *>(ledcast::dma_pool_allocate(row_bytes));
      if (row_data == nullptr) {

        ESP_LOGE("I2S-DMA", "CRITICAL ERROR: colour depth of %d bits does not fit the DMA pool (%u of %u bytes free).\r\n",
                 m_cfg.getPixelColorDepthBits(),
                 (unsigned int)(ledcast::dma_pool_capacity() - ledcast::dma_pool_used()),
                 (unsigned int)ledcast::dma_pool_capacity());

        return false;
      }

      auto& row = frame_buffer[fb].rowBits[malloc_num];
      row.emplace(PIXELS_PER_ROW, m_cfg.getPixelColorDepthBits(), row_data);
      allocated_fb_memory += row->getColorDepthSize(false);
      ++frame_buffer[fb].rows;
    }
  }
  ESP_LOGI("I2S-DMA", "Allocating %d bytes memory for DMA BCM framebuffer(s).", allocated_fb_memory);


  /***
   * Step 1: Check what the minimum refresh rate is, and calculate the lsbMsbTransitionBit
   *         which is the bit at which we can reduce the colour depth to achieve the minimum refresh rate.
   * 
   *         This also determines the number of DMA descriptors required per row.
   */  
   
  static_assert(CLKS_DURING_LATCH == 0, "Update shared DMA estimator for extra latch clocks");
  const auto memory = hub75_bcm::dma_memory(PIXELS_PER_ROW, ROWS_PER_FRAME,
      m_cfg.getPixelColorDepthBits(), fbs_required, m_cfg.i2sspeed,
      m_cfg.frame_rate, hub75_bcm::kInterleavedRows &&
      m_cfg.line_decoder != HUB75_I2S_CFG::SM5266P && m_cfg.line_decoder != HUB75_I2S_CFG::SM5368);
  lsbMsbTransitionBit = memory.transition;
  calculated_refresh_rate = memory.refresh_hz;
  passes_per_frame = memory.passes_per_frame;

  ESP_LOGI("I2S-DMA", "%d frames/s: %d passes/s, %d passes per frame, BCM transition bit %d, pad %u of %u clocks",
           m_cfg.frame_rate, calculated_refresh_rate, passes_per_frame, lsbMsbTransitionBit,
           (unsigned int)memory.pad_clocks, (unsigned int)memory.pass_clocks);

  scan_pad = nullptr;
  scan_pad_words = memory.pad_clocks;
  if (scan_pad_words != 0) {
    scan_pad = static_cast<ESP32_I2S_DMA_STORAGE_TYPE *>(ledcast::dma_pool_allocate(memory.pad_buffer_bytes));
    if (scan_pad == nullptr) {
      ESP_LOGE("I2S-DMA", "Scan pad of %u bytes does not fit the DMA pool.", (unsigned int)memory.pad_buffer_bytes);
      return false;
    }
  }

  ESP_LOGI("I2S-DMA", "DMA frame buffer color depths: %d", m_cfg.getPixelColorDepthBits());

  /***
   * Step 2:  Calculate the DMA descriptors required, which is used for memory allocation of the DMA linked list memory structure.
   *          This determines the number of passes required to shift out the colour bits in the framebuffer.
   *          We need to also take into consderation where a chain of panels (pixels) is so long, it requires more than one DMA payload,
   *          give this library's DMA output memory allocation approach is by the row.
   */
	
  const bool interleaved = hub75_bcm::kInterleavedRows &&
                           m_cfg.line_decoder != HUB75_I2S_CFG::SM5266P &&
                           m_cfg.line_decoder != HUB75_I2S_CFG::SM5368;
  const size_t plane_bytes = frame_buffer[0].rowBits[0]->getColorDepthSize(true);
  const int dma_descriptions_required = static_cast<int>(memory.descriptors_per_buffer);
  ESP_LOGV("I2S-DMA", "DMA descriptors required per buffer: %d", dma_descriptions_required);    

  /***
   * Step 3:  Allocate the DMA descriptor memory via. the relevant platform DMA implementation class.
   */

  if (m_cfg.double_buff) {
    dma_bus.enable_double_dma_desc();
  }

  if ( !dma_bus.allocate_dma_desc_memory(dma_descriptions_required) )
  {
    return false;
  }


  /***
   * Step 4:  Link up the DMA descriptors per the colour depth and rows.
   */

  //fbs_required = 1; // (m_cfg.double_buff) ? 2 : 1;
  for (int fb = 0; fb < (fbs_required); fb++)
  {  
	
	int _dmadescriptor_count = 0; // for tracking
	
    hub75_bcm::for_each_span(ROWS_PER_FRAME, m_cfg.getPixelColorDepthBits(),
                            lsbMsbTransitionBit, interleaved,
        [&](uint8_t row, uint8_t plane, uint8_t planes) {
            auto *payload = frame_buffer[fb].rowBits[row]->getDataPtr(plane);
            size_t remaining = plane_bytes * planes;
            while (remaining) {
                const size_t bytes = remaining < DMA_MAX ? remaining : DMA_MAX;
                dma_bus.create_dma_desc_link(payload, bytes, fb == 1);
                payload += bytes / sizeof(ESP32_I2S_DMA_STORAGE_TYPE);
                remaining -= bytes;
                ++_dmadescriptor_count;
            }
            scan_tail = payload - 1;
        });

    // Добивка прохода до точного периода — в хвосте кольца, после последнего
    // плана: флип переставляет ссылку последнего дескриптора, и граница кадра
    // остаётся на первом дескрипторе кольца.
    size_t pad_remaining = scan_pad_words * sizeof(ESP32_I2S_DMA_STORAGE_TYPE);
    while (pad_remaining) {
        const size_t bytes = pad_remaining < DMA_MAX ? pad_remaining : DMA_MAX;
        dma_bus.create_dma_desc_link(scan_pad, bytes, fb == 1);
        pad_remaining -= bytes;
        ++_dmadescriptor_count;
    }
	
    ESP_LOGI("I2S-DMA", "Created %d DMA descriptors for buffer %d.", _dmadescriptor_count, fb);	
    if (_dmadescriptor_count != dma_descriptions_required) {
      ESP_LOGE("I2S-DMA", "Ring %d has %d descriptors, the estimator expected %d.",
               fb, _dmadescriptor_count, dma_descriptions_required);
      return false;
    }
	
  } // end framebuffer loop
	  

  /***
   * Step 5:  Set default framebuffer to fb[0]
   */  

  fb = &frame_buffer[0];
  

  //
  //    Setup DMA and Output to GPIO
  //
  auto bus_cfg = dma_bus.config(); // バス設定用の構造体を取得します。

  bus_cfg.bus_freq    = m_cfg.i2sspeed;
  bus_cfg.pin_wr      = m_cfg.gpio.clk;
  bus_cfg.invert_pclk = m_cfg.clkphase;
  // Раскладка бит маски совпадает с BIT_*, потому что номер линии данных равен
  // номеру бита в слове DMA.
  bus_cfg.invert_data_mask = static_cast<uint16_t>((m_cfg.invert_rgb ? 0x003F : 0) |
                                                   (m_cfg.oe_active_high ? BIT_OE : 0));

  bus_cfg.pin_d0 = m_cfg.gpio.r1;
  bus_cfg.pin_d1 = m_cfg.gpio.g1;
  bus_cfg.pin_d2 = m_cfg.gpio.b1;
  bus_cfg.pin_d3 = m_cfg.gpio.r2;
  bus_cfg.pin_d4 = m_cfg.gpio.g2;
  bus_cfg.pin_d5 = m_cfg.gpio.b2;
  bus_cfg.pin_d6 = m_cfg.gpio.lat;
  bus_cfg.pin_d7 = m_cfg.gpio.oe;
  bus_cfg.pin_d8 = m_cfg.gpio.a;
  bus_cfg.pin_d9 = m_cfg.gpio.b;
  bus_cfg.pin_d10 = m_cfg.gpio.c;
  bus_cfg.pin_d11 = m_cfg.gpio.d;
  bus_cfg.pin_d12 = m_cfg.gpio.e;
  bus_cfg.pin_d13 = -1;
  bus_cfg.pin_d14 = -1;
  bus_cfg.pin_d15 = -1;

  dma_bus.config(bus_cfg);

  ESP_LOGI("I2S-DMA", "DMA setup completed");
  
  initialized = true;

  return true;

} // end setupDMA

/* There are 'bits' set in the frameStruct that we simply don't need to set every single time we change a pixel / DMA buffer co-ordinate.
 *  For example, the bits that determine the address lines, we don't need to set these every time. Once they're in place, and assuming we
 *  don't accidentally clear them, then we don't need to set them again.
 *  So to save processing, we strip this logic out to the absolute bare minimum, which is toggling only the R,G,B pixels (bits) per co-ord.
 *
 *  Critical dependency: That 'updateMatrixDMABuffer(uint8_t red, uint8_t green, uint8_t blue)' has been run at least once over the
 *                       entire frameBuffer to ensure all the non R,G,B bitmasks are in place (i.e. like OE, Address Lines etc.)
 *
 *  Note: If you change the brightness with setBrightness() you MUST then clearScreen() and repaint / flush the entire framebuffer.
 */

/** @brief - Update pixel at specific co-ordinate in the DMA buffer
 *  this is the main method used to update DMA buffer on pixel-by-pixel level so it must be fast, real fast!
 *  Let's put it into IRAM to avoid situations when it could be flushed out of instruction cache
 *  and had to be read from spi-flash over and over again.
 *  Yes, it is always a tradeoff between memory/speed/size, but compared to DMA-buffer size is not a big deal
 *
 *  Note: Cannot pass a negative co-ord as it makes no sense in the DMA bit array lookup.
 */
void IRAM_ATTR MatrixPanel_I2S_DMA::updateMatrixDMABuffer(uint16_t x_coord, uint16_t y_coord, uint8_t red, uint8_t green, uint8_t blue)
{
  if (!initialized)
    return;

  /* 1) Check that the co-ordinates are within range, or it'll break everything big time.
   * Valid co-ordinates are from 0 to (MATRIX_XXXX-1)
   */
  if (x_coord >= PIXELS_PER_ROW || y_coord >= m_cfg.mx_height)
  {
    return;
  }

  SCALE_TO_COLOUR_DEPTH()

  /* When using the drawPixel, we are obviously only changing the value of one x,y position,
   * however, the two-scan panels paint TWO lines at the same time
   * and this reflects the parallel in-DMA-memory data structure of uint16_t's that are getting
   * pumped out at high speed.
   *
   * So we need to ensure we persist the bits (8 of them) of the uint16_t for the row we aren't changing.
   *
   * The DMA buffer order has also been reversed (refer to the last code in this function)
   * so we have to check for this and check the correct position of the MATRIX_DATA_STORAGE_TYPE
   * data.
   */
  uint16_t _colourbitclear = BITMASK_RGB1_CLEAR, _colourbitoffset = 0;

  if (y_coord >= ROWS_PER_FRAME)
  { // if we are drawing to the bottom part of the panel
    _colourbitoffset = BITS_RGB2_OFFSET;
    _colourbitclear = BITMASK_RGB2_CLEAR;
    y_coord -= ROWS_PER_FRAME;
  }

  // Iterating through colour depth bits, which we assume are 8 bits per RGB subpixel (24bpp)
  uint8_t colour_depth_idx = m_cfg.getPixelColorDepthBits();
  do
  {
    --colour_depth_idx;

    // Extract bit at current depth index
    uint16_t mask = (1 << colour_depth_idx);
    uint16_t RGB_output_bits = 0;

    /* Per the .h file, the order of the output RGB bits is:
     * BIT_B2, BIT_G2, BIT_R2,    BIT_B1, BIT_G1, BIT_R1     */
    RGB_output_bits |= (bool)(blue_val & mask); // --B
    RGB_output_bits <<= 1;
    RGB_output_bits |= (bool)(green_val & mask); // -BG
    RGB_output_bits <<= 1;
    RGB_output_bits |= (bool)(red_val & mask); // BGR
    RGB_output_bits <<= _colourbitoffset;    // shift colour bits to the required position

    // Get the contents at this address,
    // it would represent a vector pointing to the full row of pixels for the specified colour depth bit at Y coordinate
    ESP32_I2S_DMA_STORAGE_TYPE *p = getRowDataPtr(y_coord, colour_depth_idx);

    // We need to update the correct uint16_t word in the rowBitStruct array pointing to a specific pixel at X - coordinate
    p[x_coord] &= _colourbitclear; // reset RGB bits
    p[x_coord] |= RGB_output_bits; // set new RGB bits

#if defined(SPIRAM_DMA_BUFFER)
    Cache_WriteBack_Addr((uint32_t)&p[x_coord], sizeof(ESP32_I2S_DMA_STORAGE_TYPE));
#endif

  } while (colour_depth_idx); // end of colour depth loop (8)
} // updateMatrixDMABuffer (specific co-ords change)


/* Update the entire buffer with a single specific colour - quicker */

// Прямая адресация строк: у таких панелей каждая строка висит на своей линии
// A..D, выбранная линия притянута к нулю. Признак — одна точка зажигает
// столбик светодиодов при обычной адресации через дешифратор.
//
// Отсюда предел в четыре адресные строки: номер строки становится сдвигом
// единицы, и с четвёртой строки бит вышел бы за маску. Такую раскладку не
// пропускает config_validate — сюда она не доходит.
static inline ESP32_I2S_DMA_STORAGE_TYPE hub75_apply_line_decoder(
    HUB75_I2S_CFG::line_driver decoder, ESP32_I2S_DMA_STORAGE_TYPE abcde, int rows_per_frame)
{
  return hub75_bcm::parallel_row_address(abcde, rows_per_frame,
                                        decoder == HUB75_I2S_CFG::TYPE_DIRECT);
}

void MatrixPanel_I2S_DMA::updateMatrixDMABuffer(uint8_t red, uint8_t green, uint8_t blue)
{
  if (!initialized)
    return;

  SCALE_TO_COLOUR_DEPTH()

  for (uint8_t colour_depth_idx = 0; colour_depth_idx < m_cfg.getPixelColorDepthBits(); colour_depth_idx++) // colour depth - 8 iterations
  {
    // let's precalculate RGB1 and RGB2 bits than flood it over the entire DMA buffer
    uint16_t RGB_output_bits = 0;

    // Extract bit at current depth index
    uint16_t mask = (1 << colour_depth_idx);

    /* Per the .h file, the order of the output RGB bits is:
     * BIT_B2, BIT_G2, BIT_R2,    BIT_B1, BIT_G1, BIT_R1      */
    RGB_output_bits |= (bool)(blue_val & mask); // --B
    RGB_output_bits <<= 1;
    RGB_output_bits |= (bool)(green_val & mask); // -BG
    RGB_output_bits <<= 1;
    RGB_output_bits |= (bool)(red_val & mask); // BGR

    // Duplicate and shift across so we have have 6 populated bits of RGB1 and RGB2 pin values suitable for DMA buffer
    RGB_output_bits |= RGB_output_bits << BITS_RGB2_OFFSET; // BGRBGR

    // Serial.printf("Fill with: 0x%#06x\n", RGB_output_bits);

    // iterate rows
    int matrix_frame_parallel_row = fb->rows;
    do
    {
      --matrix_frame_parallel_row;

      // The destination for the pixel row bitstream
      ESP32_I2S_DMA_STORAGE_TYPE *p = getRowDataPtr(matrix_frame_parallel_row, colour_depth_idx);

      // iterate pixels in a row
      int x_coord = fb->rowBits[matrix_frame_parallel_row]->width;
      do
      {
        --x_coord;
        p[x_coord] &= BITMASK_RGB12_CLEAR; // reset colour bits
        p[x_coord] |= RGB_output_bits;     // set new colour bits

#if defined(SPIRAM_DMA_BUFFER)
        Cache_WriteBack_Addr((uint32_t)&p[x_coord], sizeof(ESP32_I2S_DMA_STORAGE_TYPE));
#endif

      } while (x_coord);

    } while (matrix_frame_parallel_row); // end row iteration
  }                                      // colour depth loop (8)
} // updateMatrixDMABuffer (full frame paint)

/**
 * @brief - clears and reinitializes colour/control data in DMA buffs
 * When allocated, DMA buffs might be dirty, so we need to blank it and initialize ABCDE,LAT,OE control bits.
 * Those control bits are constants during the entire DMA sweep and never changed when updating just pixel colour data
 * so we could set it once on DMA buffs initialization and forget.
 * This effectively clears buffers to blank BLACK and makes it ready to display output.
 * (Brightness control via OE bit manipulation is another case) - this must be done as well seperately!
 */
void MatrixPanel_I2S_DMA::clearFrameBuffer(bool _buff_id)
{
  if (!initialized)
    return;

  frameStruct *fb = &frame_buffer[_buff_id];

  // we start with iterating all rows in dma_buff structure
  int row_idx = fb->rows;
  do
  {
    --row_idx;

    ESP32_I2S_DMA_STORAGE_TYPE *row = fb->rowBits[row_idx]->getDataPtr(0); // set pointer to the HEAD of a buffer holding data for the entire matrix row
    ESP32_I2S_DMA_STORAGE_TYPE abcde =
        hub75_apply_line_decoder(m_cfg.line_decoder, (ESP32_I2S_DMA_STORAGE_TYPE)row_idx, ROWS_PER_FRAME);

    // get last pixel index in a row of all colourdepths
    int x_pixel = fb->rowBits[row_idx]->width * fb->rowBits[row_idx]->colour_depth;

	abcde <<= BITS_ADDR_OFFSET; // shift row y-coord to match ABCDE bits in vector from 8 to 12
	do
	{
		--x_pixel;
		if (m_cfg.line_decoder == HUB75_I2S_CFG::SM5266P)
		{
			// modifications here for row shift register type SM5266P
			// https://github.com/mrfaptastic/ESP32-HUB75-MatrixPanel-I2S-DMA/issues/164
			row[x_pixel] = abcde & (0x18 << BITS_ADDR_OFFSET); // mask out the bottom 3 bits which are the clk di bk inputs
		}
		else if (m_cfg.line_decoder  == HUB75_I2S_CFG::SM5368) 
		{
			row[x_pixel] = 0x0000;
		}
		else
		{
            const auto plane = x_pixel / fb->rowBits[row_idx]->width;
            const auto lit_row = hub75_bcm::illuminated_row(
                row_idx, plane, ROWS_PER_FRAME, lsbMsbTransitionBit, hub75_bcm::kInterleavedRows);
            row[x_pixel] = hub75_apply_line_decoder(
                m_cfg.line_decoder, lit_row, ROWS_PER_FRAME) << BITS_ADDR_OFFSET;
		}

	} while (x_pixel != fb->rowBits[row_idx]->width); // spare the first "width's" worth of pixels as they are the LSB pixels/colordepth

	// Plane zero always shifts the next row while exposing the previous row.
	if (row_idx == 0) { 
		abcde = ROWS_PER_FRAME-1; // wrap around
	} else {
		abcde = row_idx-1;	
	}
	abcde = hub75_apply_line_decoder(m_cfg.line_decoder, abcde, ROWS_PER_FRAME);

    abcde <<= BITS_ADDR_OFFSET; // shift row y-coord to match ABCDE bits in vector from 8 to 12		
	do
    {
      --x_pixel;

      if (m_cfg.line_decoder == HUB75_I2S_CFG::SM5266P)
      {
        // modifications here for row shift register type SM5266P
        // https://github.com/mrfaptastic/ESP32-HUB75-MatrixPanel-I2S-DMA/issues/164
        row[x_pixel] = abcde & (0x18 << BITS_ADDR_OFFSET); // mask out the bottom 3 bits which are the clk di bk inputs
      }
      else if (m_cfg.line_decoder  == HUB75_I2S_CFG::SM5368) 
      {
        row[x_pixel] = 0x0000;
      }
      else
      {
        row[x_pixel] = abcde;
      }

    } while (x_pixel);

    // modifications here for row shift register type SM5266P
    // https://github.com/mrfaptastic/ESP32-HUB75-MatrixPanel-I2S-DMA/issues/164
    if (m_cfg.line_decoder == HUB75_I2S_CFG::SM5266P)
    {
      uint16_t serialCount;
      uint16_t latch;
      x_pixel = fb->rowBits[row_idx]->width - 16; // come back 8*2 pixels to allow for 8 writes
      serialCount = 8;
      do
      {
        serialCount--;
        latch = row[x_pixel] | (((((ESP32_I2S_DMA_STORAGE_TYPE)row_idx) % 8) == serialCount) << 1) << BITS_ADDR_OFFSET; // data on 'B'
        row[x_pixel++] = latch | (0x05 << BITS_ADDR_OFFSET);                                                            // clock high on 'A'and BK high for update
        row[x_pixel++] = latch | (0x04 << BITS_ADDR_OFFSET);                                                            // clock low on 'A'and BK high for update
      } while (serialCount);
    } // end SM5266P

    // row selection for SM5368 shift regs with ABC-only addressing. A is row clk, B is BK and C is row data
    if (m_cfg.line_decoder == HUB75_I2S_CFG::SM5368) 
    {
      x_pixel = fb->rowBits[row_idx]->width - 1;                                                                        // last pixel in first block)
      uint16_t c = (row_idx == 0) ? BIT_C : 0x0000;                                                                     // set row data (C) when row==0, then push through shift regs for all other rows
      row[x_pixel - 1] |= c | BIT_B;                                                            // set row data
      row[x_pixel + 0] |= c | BIT_A | BIT_B;                                             // set row clk and bk, carry row data
    } // end DP3246_SM5368

    // let's set LAT/OE control bits for specific pixels in each colour_index subrows
    // Need to consider the original ESP32's (WROOM) DMA TX FIFO reordering of bytes...
    uint8_t colouridx = fb->rowBits[row_idx]->colour_depth;
    do
    {
      --colouridx;

      // switch pointer to a row for a specific colour index
      row = fb->rowBits[row_idx]->getDataPtr(colouridx);

      const bool parallel = m_cfg.line_decoder != HUB75_I2S_CFG::SM5266P &&
                            m_cfg.line_decoder != HUB75_I2S_CFG::SM5368;
      const uint8_t guard = hub75_bcm::control_blanking(m_cfg.latch_blanking,
          m_cfg.driver == HUB75_I2S_CFG::DP3246 ? 3 : 1, parallel);
      if (parallel) {
        const auto next_address = hub75_apply_line_decoder(
            m_cfg.line_decoder, row_idx, ROWS_PER_FRAME) << BITS_ADDR_OFFSET;
        for (unsigned x = hub75_bcm::address_switch_clock(fb->rowBits[row_idx]->width, guard);
             x < fb->rowBits[row_idx]->width; ++x)
          row[x] = (row[x] & ~(0x1fu << BITS_ADDR_OFFSET)) | next_address;
      }

      // DP3246 needs the latch high for 3 clock cycles, so start 2 cycles earlier
      if (m_cfg.driver == HUB75_I2S_CFG::DP3246) 
      {
        row[fb->rowBits[row_idx]->width - 3] |= BIT_LAT;   // DP3246 needs 3 clock cycle latch 
        row[fb->rowBits[row_idx]->width - 2] |= BIT_LAT;   // DP3246 needs 3 clock cycle latch 
      } // DP3246_SM5368
      
      row[fb->rowBits[row_idx]->width - 1] |= BIT_LAT; // -1 pixel to compensate array index starting at 0

      // need to disable OE before/after latch to hide row transition
      // Should be one clock or more before latch, otherwise can get ghosting
      uint8_t _blank = guard;
      do
      {
        --_blank;

        row[0 + _blank] |= BIT_OE;                               // disable output
        row[fb->rowBits[row_idx]->width - 1] |= BIT_OE;          // disable output
        row[fb->rowBits[row_idx]->width - _blank - 1] |= BIT_OE; // (LAT pulse is (width-2) -1 pixel to compensate array index starting at 0

      } while (_blank);

    } while (colouridx);

#if defined(SPIRAM_DMA_BUFFER)
    Cache_WriteBack_Addr((uint32_t)row, fb->rowBits[row_idx]->getColorDepthSize(false));
#endif

  } while (row_idx);
}

void MatrixPanel_I2S_DMA::setBrightnessOE(uint8_t brt, const int _buff_id)
{

  if (!initialized)
    return;

  frameStruct *fb = &frame_buffer[_buff_id];

  const bool parallel = m_cfg.line_decoder != HUB75_I2S_CFG::SM5266P &&
                        m_cfg.line_decoder != HUB75_I2S_CFG::SM5368;
  uint8_t _blank = hub75_bcm::control_blanking(m_cfg.latch_blanking,
      m_cfg.driver == HUB75_I2S_CFG::DP3246 ? 3 : 1, parallel);
  uint8_t _depth = fb->rowBits[0]->colour_depth;
  uint16_t _width = fb->rowBits[0]->width;

  // start with iterating all rows in dma_buff structure
  int row_idx = fb->rows;
  do
  {
    --row_idx;

    // let's set OE control bits for specific pixels in each color_index subrows
    uint8_t colouridx = _depth;
    do
    {
      --colouridx;

      // RGB shifts into a separate register while OE exposes the PREVIOUS
      // latched plane. LAT at the end of this payload switches to colouridx.
      // Plane zero exposes the preceding row's last plane at full weight;
      // low-plane OE windows are shifted by one relative to the RGB data.
      int rightshift = hub75_bcm::oe_shift(colouridx, lsbMsbTransitionBit);

      // Ширина окна для этого плана, затем масштабирование на яркость (0-255).
      int max_pixels_for_bitplane = (_width - _blank) >> rightshift;
      int brightness_in_x_pixels = (max_pixels_for_bitplane * brt) >> 8;

      // Пол в одну точку — только планам с полным окном, иначе на малой яркости
      // все планы получили бы по одной точке и веса снова сравнялись бы.
      if (brt > 0 && rightshift == 0 && brightness_in_x_pixels == 0) {
        brightness_in_x_pixels = 1;
      }

      // Safety margin: Ensure we never exceed max_pixels - 1 to maintain blanking headroom.
      // At extreme brightness (252-255), we need at least (_blank + 1) total blanking pixels
      // to prevent ghosting and artifacts, especially with high pixel density (many white pixels).
      if (brightness_in_x_pixels > max_pixels_for_bitplane - 1) {
        brightness_in_x_pixels = max_pixels_for_bitplane - 1;
      }

      // Окно сузилось до нуля: вес этого плана мельче, чем одна точка строки, и
      // он просто не светит. Отрицательная ширина ниже развернулась бы в мусор.
      if (brightness_in_x_pixels < 0) {
        brightness_in_x_pixels = 0;
      }

      // switch pointer to a row for a specific color index
      ESP32_I2S_DMA_STORAGE_TYPE *row = fb->rowBits[row_idx]->getDataPtr(colouridx);

      // define range of Output Enable on the center of the row
      const auto window = hub75_bcm::oe_window(_width, _blank, brightness_in_x_pixels);
      int x_coord = _width;
      do
      {
        --x_coord;

        // (the check is already including "blanking" )
        if (x_coord >= window.begin && x_coord < window.end)
        {
          row[x_coord] &= BITMASK_OE_CLEAR;
        }
        else
        {
          row[x_coord] |= BIT_OE; // Disable output after this point.
        }

      } while (x_coord);

    } while (colouridx);

#if defined(SPIRAM_DMA_BUFFER)
	// Force the flush and update of the PSRAM for the memory address range of the 'row data' as
	// data changes probably aren't being sent out via DMA as they're sitting in a hadrware 'cache' 
    ESP32_I2S_DMA_STORAGE_TYPE *row_ptr = fb->rowBits[row_idx]->getDataPtr(0);
    Cache_WriteBack_Addr((uint32_t)row_ptr, fb->rowBits[row_idx]->getColorDepthSize(false));
#endif
  } while (row_idx);
}


/*
 *  overload for compatibility
 */

bool MatrixPanel_I2S_DMA::begin(int r1, int g1, int b1, int r2, int g2, int b2, int a, int b, int c, int d, int e, int lat, int oe, int clk)
{
  if (initialized)
    return true;
  // RGB
  m_cfg.gpio.r1 = r1;
  m_cfg.gpio.g1 = g1;
  m_cfg.gpio.b1 = b1;
  m_cfg.gpio.r2 = r2;
  m_cfg.gpio.g2 = g2;
  m_cfg.gpio.b2 = b2;

  // Line Select
  m_cfg.gpio.a = a;
  m_cfg.gpio.b = b;
  m_cfg.gpio.c = c;
  m_cfg.gpio.d = d;
  m_cfg.gpio.e = e;

  // Clock & Control
  m_cfg.gpio.lat = lat;
  m_cfg.gpio.oe = oe;
  m_cfg.gpio.clk = clk;

  return begin();
}

bool MatrixPanel_I2S_DMA::begin(const HUB75_I2S_CFG &cfg)
{
  if (initialized)
    return true;

  if (!setCfg(cfg))
    return false;

  return begin();
}

/**
 * @brief - Sets how many clock cycles to blank OE before/after LAT signal change
 * @param uint8_t pulses - clocks before/after OE
 * default is DEFAULT_LAT_BLANKING
 * Max is MAX_LAT_BLANKING
 * @returns - new value for m_cfg.latch_blanking
 */
uint8_t MatrixPanel_I2S_DMA::setLatBlanking(uint8_t pulses)
{
  if (pulses > MAX_LAT_BLANKING)
    pulses = MAX_LAT_BLANKING;

  if (!pulses)
    pulses = DEFAULT_LAT_BLANKING;

  m_cfg.latch_blanking = pulses;

  // remove brightness var for now.
  // setPanelBrightness(brightness);    // set brightness to reset OE bits to the values matching new LAT blanking setting
  return m_cfg.latch_blanking;
}

#ifndef NO_FAST_FUNCTIONS
/**
 * @brief - update DMA buff drawing horizontal line at specified coordinates
 * @param x_coord - line start coordinate x
 * @param y_coord - line start coordinate y
 * @param l - line length
 * @param r,g,b, - RGB888 colour
 */
void MatrixPanel_I2S_DMA::hlineDMA(int16_t x_coord, int16_t y_coord, int16_t l, uint8_t red, uint8_t green, uint8_t blue)
{
  if (!initialized)
    return;

  if ((x_coord + l) < 1 || y_coord < 0 || l < 1 || x_coord >= PIXELS_PER_ROW || y_coord >= m_cfg.mx_height)
    return;

  l = x_coord < 0 ? l + x_coord : l;
  x_coord = x_coord < 0 ? 0 : x_coord;

  l = ((x_coord + l) >= PIXELS_PER_ROW) ? (PIXELS_PER_ROW - x_coord) : l;

  // if (x_coord+l > PIXELS_PER_ROW)
  //    l = PIXELS_PER_ROW - x_coord + 1;     // reset width to end of row

  SCALE_TO_COLOUR_DEPTH()

  uint16_t _colourbitclear = BITMASK_RGB1_CLEAR, _colourbitoffset = 0;

  if (y_coord >= ROWS_PER_FRAME)
  { // if we are drawing to the bottom part of the panel
    _colourbitoffset = BITS_RGB2_OFFSET;
    _colourbitclear = BITMASK_RGB2_CLEAR;
    y_coord -= ROWS_PER_FRAME;
  }

  // Iterating through colour depth bits (8 iterations)
  uint8_t colour_depth_idx = m_cfg.getPixelColorDepthBits();
  do
  {
    --colour_depth_idx;

    // let's precalculate RGB1 and RGB2 bits than flood it over the entire DMA buffer
    uint16_t RGB_output_bits = 0;

    // Extract bit at current depth index
    uint16_t mask = (1 << colour_depth_idx);

    /* Per the .h file, the order of the output RGB bits is:
     * BIT_B2, BIT_G2, BIT_R2,    BIT_B1, BIT_G1, BIT_R1     */
    RGB_output_bits |= (bool)(blue_val & mask); // --B
    RGB_output_bits <<= 1;
    RGB_output_bits |= (bool)(green_val & mask); // -BG
    RGB_output_bits <<= 1;
    RGB_output_bits |= (bool)(red_val & mask); // BGR
    RGB_output_bits <<= _colourbitoffset;    // shift color bits to the required position

    // Get the contents at this address,
    // it would represent a vector pointing to the full row of pixels for the specified colour depth bit at Y coordinate
    ESP32_I2S_DMA_STORAGE_TYPE *p = fb->rowBits[y_coord]->getDataPtr(colour_depth_idx);
    // inlined version works slower here, dunno why :(
    // ESP32_I2S_DMA_STORAGE_TYPE *p = getRowDataPtr(y_coord, colour_depth_idx, back_buffer_id);

    int16_t _l = l;
    do
    { // iterate pixels in a row
      int16_t _x = x_coord + --_l;

      uint16_t &v = p[_x];

      v &= _colourbitclear;   // reset colour bits
      v |= RGB_output_bits;   // set new colour bits
    } while (_l);             // iterate pixels in a row
  } while (colour_depth_idx); // end of colour depth loop (8)
} // hlineDMA()

/**
 * @brief - update DMA buff drawing vertical line at specified coordinates
 * @param x_coord - line start coordinate x
 * @param y_coord - line start coordinate y
 * @param l - line length
 * @param r,g,b, - RGB888 colour
 */
void MatrixPanel_I2S_DMA::vlineDMA(int16_t x_coord, int16_t y_coord, int16_t l, uint8_t red, uint8_t green, uint8_t blue)
{
  if (!initialized)
    return;

  if (x_coord < 0 || (y_coord + l) < 1 || l < 1 || x_coord >= PIXELS_PER_ROW || y_coord >= m_cfg.mx_height)
    return;

  l = y_coord < 0 ? l + y_coord : l;
  y_coord = y_coord < 0 ? 0 : y_coord;

  // check for a length that goes beyond the height of the screen! Array out of bounds dma memory changes = screwed output #163
  l = ((y_coord + l) >= m_cfg.mx_height) ? (m_cfg.mx_height - y_coord) : l;
  // if (y_coord + l > m_cfg.mx_height)
  ///    l = m_cfg.mx_height - y_coord + 1;     // reset width to end of col

  SCALE_TO_COLOUR_DEPTH()

  uint8_t colour_depth_idx = m_cfg.getPixelColorDepthBits();
  do
  { // Iterating through colour depth bits (8 iterations)
    --colour_depth_idx;

    // Extract bit at current depth index
    uint16_t mask = (1 << colour_depth_idx);
    uint16_t RGB_output_bits = 0;

    /* Per the .h file, the order of the output RGB bits is:
     * BIT_B2, BIT_G2, BIT_R2,    BIT_B1, BIT_G1, BIT_R1   */
    RGB_output_bits |= (bool)(blue_val & mask); // --B
    RGB_output_bits <<= 1;
    RGB_output_bits |= (bool)(green_val & mask); // -BG
    RGB_output_bits <<= 1;
    RGB_output_bits |= (bool)(red_val & mask); // BGR

    int16_t _l = 0, _y = y_coord;
    uint16_t _colourbitclear = BITMASK_RGB1_CLEAR;
    do
    { // iterate pixels in a column

      if (_y >= ROWS_PER_FRAME)
      { // if y-coord overlapped bottom-half panel
        _y -= ROWS_PER_FRAME;
        _colourbitclear = BITMASK_RGB2_CLEAR;
        RGB_output_bits <<= BITS_RGB2_OFFSET;
      }

      // Get the contents at this address,
      // it would represent a vector pointing to the full row of pixels for the specified colour depth bit at Y coordinate
      // ESP32_I2S_DMA_STORAGE_TYPE *p = getRowDataPtr(_y, colour_depth_idx, back_buffer_id);
      ESP32_I2S_DMA_STORAGE_TYPE *p = fb->rowBits[_y]->getDataPtr(colour_depth_idx);

      p[x_coord] &= _colourbitclear; // reset RGB bits
      p[x_coord] |= RGB_output_bits; // set new RGB bits
      ++_y;
    } while (++_l != l);      // iterate pixels in a col
  } while (colour_depth_idx); // end of colour depth loop (8)
} // vlineDMA()

/**
 * @brief - update DMA buff drawing a rectangular at specified coordinates
 * this works much faster than multiple consecutive per-pixel calls to updateMatrixDMABuffer()
 * @param int16_t x, int16_t y - coordinates of a top-left corner
 * @param int16_t w, int16_t h - width and height of a rectangular, min is 1 px
 * @param uint8_t r - RGB888 colour
 * @param uint8_t g - RGB888 colour
 * @param uint8_t b - RGB888 colour
 */
void MatrixPanel_I2S_DMA::fillRectDMA(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t r, uint8_t g, uint8_t b)
{

  // h-lines are >2 times faster than v-lines
  // so will use it only for tall rects with h >2w
  if (h > 2 * w)
  {
    // draw using v-lines
    do
    {
      --w;
      vlineDMA(x + w, y, h, r, g, b);
    } while (w);
  }
  else
  {
    // draw using h-lines
    do
    {
      --h;
      hlineDMA(x, y + h, w, r, g, b);
    } while (h);
  }
}

#endif // NO_FAST_FUNCTIONS

// Слово добивки берёт адресные линии из последнего слова прохода, а не
// вычисляет их: у последовательных дешифраторов (SM5266P, SM5368) адрес —
// это данные сдвигового регистра, и любой фронт на них сдвинул бы строку.
// Постоянный уровень фронтов не даёт. OE погашен, LAT и RGB в нуле.
void MatrixPanel_I2S_DMA::fillScanPad()
{
  if (scan_pad == nullptr || scan_tail == nullptr) return;
  constexpr ESP32_I2S_DMA_STORAGE_TYPE kAddress = BIT_A | BIT_B | BIT_C | BIT_D | BIT_E;
  const auto idle = static_cast<ESP32_I2S_DMA_STORAGE_TYPE>((*scan_tail & kAddress) | BIT_OE);
  const size_t words = std::min<size_t>(scan_pad_words, DMA_MAX / sizeof(ESP32_I2S_DMA_STORAGE_TYPE));
  for (size_t i = 0; i < words; ++i) scan_pad[i] = idle;
}
