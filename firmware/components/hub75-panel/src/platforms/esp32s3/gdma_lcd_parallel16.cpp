/*********************************************************************************************
  Simple example of using the ESP32-S3's LCD peripheral for general-purpose
  (non-LCD) parallel data output with DMA. Connect 8 LEDs (or logic analyzer),
  cycles through a pattern among them at about 1 Hz.
  This code is ONLY for the ESP32-S3, NOT the S2, C3 or original ESP32.
  None of this is authoritative canon, just a lot of trial error w/datasheet
  and register poking. Probably more robust ways of doing this still TBD.


 FULL CREDIT goes to AdaFruit and  https://github.com/PaintYourDragon
 
 https://blog.adafruit.com/2022/06/21/esp32uesday-more-s3-lcd-peripheral-hacking-with-code/
 
 https://github.com/adafruit/Adafruit_Protomatter/blob/master/src/arch/esp32-s3.h

 PLEASE SUPPORT THEM!

 ********************************************************************************************/
#if __has_include (<hal/lcd_ll.h>)
// Stop compile errors: /src/platforms/esp32s3/gdma_lcd_parallel16.hpp:64:10: fatal error: hal/lcd_ll.h: No such file or directory
  #pragma message "Compiling for ESP32-S3"

  #ifdef ARDUINO_ARCH_ESP32
     #include <Arduino.h>
  #endif

  #include "gdma_lcd_parallel16.hpp"
  #include "dmapool.h"
  #include "bcm_memory.h"
  static_assert(sizeof(HUB75_DMA_DESCRIPTOR_T) == hub75_bcm::kDescriptorBytes,
                "Shared configuration estimator must match GDMA descriptor size");
  static_assert(DMA_MAX == hub75_bcm::kMaxPayloadBytes,
                "Shared configuration estimator must match GDMA payload limit");
  #include "flip_wait_timing.h"
  #include "esp_attr.h"
  #include "esp_idf_version.h"


#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
  #include "esp_private/gpio.h"
#endif

  // Тактовая частота LCD_CAM: источник 160 МГц делится на lcd_clkm_div_num, а
  // lcd_clk_equ_sysclk = 1 отдаёт результат на выход как есть.
  static constexpr uint32_t kLcdSourceHz = 160000000UL;

  // Извещение о завершении первого дескриптора кольца.
  //
  // Отсюда убрана задержка в сто микросекунд из образца Adafruit: там она
  // давала данным дойти до панели прежде, чем вывод остановят. Останавливать
  // нечего — кольцо крутится всё время работы, — а задержка в обработчике
  // прерывания стоит ровно столько, сколько занимает.
  //
  // Обработчик ничего не решает: он только отмечает событие. Что оно значит,
  // разбирается flip_dma_output_buffer уже в задаче.
  IRAM_ATTR bool Bus_Parallel16::eof_isr(gdma_channel_handle_t, gdma_event_data_t* event, void* user_data)
  {
    Bus_Parallel16* bus = static_cast<Bus_Parallel16*>(user_data);
    // ESP-IDF gdma_event_data_t::tx_eof_desc_addr identifies the completed
    // EOF descriptor. Unlike OUT_DSCR_BF0 ("current pre-read descriptor" in
    // the S3 TRM), EOF proves that its payload has been read from memory.
    const uintptr_t completed = static_cast<uintptr_t>(event->tx_eof_desc_addr);
    int ring = -1;
    if (completed == reinterpret_cast<uintptr_t>(bus->_dmadesc_a)) ring = 0;
    else if (completed == reinterpret_cast<uintptr_t>(bus->_dmadesc_b)) ring = 1;
    if (ring < 0) return false;
    __atomic_add_fetch(&bus->_completed_markers[ring], 1u, __ATOMIC_RELEASE);
    BaseType_t higher_priority_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(bus->_eof_sem, &higher_priority_task_woken);
    return higher_priority_task_woken == pdTRUE;
  }

  lcd_cam_dev_t* getDev()
  {
    return &LCD_CAM;
  }

  // ------------------------------------------------------------------------------

  void Bus_Parallel16::config(const config_t& cfg)
  {
    _cfg = cfg;
    //auto port = cfg.port;
    _dev = getDev();
  }


 //https://github.com/adafruit/Adafruit_Protomatter/blob/master/src/arch/esp32-s3.h
 bool Bus_Parallel16::init(void)
 {
    ///dmabuff2 = (uint16_t*)heap_caps_malloc(sizeof(uint16_t) * 64*32, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    // LCD_CAM peripheral isn't enabled by default -- MUST begin with this:
    periph_module_enable(PERIPH_LCD_CAM_MODULE);
    _peripheral_enabled = true;
    periph_module_reset(PERIPH_LCD_CAM_MODULE);

    // Reset LCD bus
    LCD_CAM.lcd_user.lcd_reset = 1;
    esp_rom_delay_us(1000);


    // Configure LCD clock. Since this program generates human-perceptible
    // output and not data for LED matrices or NeoPixels, use almost the
    // slowest LCD clock rate possible. The S3-mini module used on Feather
    // ESP32-S3 has a 40 MHz crystal. A 2-stage clock division of 1:16000
    // is applied (250*64), yielding 2,500 Hz. Still much too fast for
    // human eyes, so later we set up the data to repeat each output byte
    // many times over.
    //LCD_CAM.lcd_clock.clk_en = 0;             // Enable peripheral clock

    // LCD_CAM_LCD_CLK_SEL Select LCD module source clock. 0: clock source is disabled. 1: XTAL_CLK. 2: PLL_D2_CLK. 3: PLL_F160M_CLK. (R/W)
    LCD_CAM.lcd_clock.lcd_clk_sel = 3;        // Use 160Mhz Clock Source -> PLL_F160M_CLK
    
    LCD_CAM.lcd_clock.lcd_ck_out_edge = 0;    // PCLK low in 1st half cycle
    LCD_CAM.lcd_clock.lcd_ck_idle_edge = 0;   // PCLK low idle
    
    LCD_CAM.lcd_clock.lcd_clkcnt_n = 1; // Should never be zero
    
    //LCD_CAM.lcd_clock.lcd_clk_equ_sysclk = 0; // PCLK = CLK / (CLKCNT_N+1)    
    LCD_CAM.lcd_clock.lcd_clk_equ_sysclk = 1; // PCLK = CLK / 1 (... so 160Mhz still)


    // Шина — ровно 16 или 20 МГц: 160 МГц делятся на 10 или на 8 без
    // дробной части, и расчёт развёртки (hub75_bcm::dma_memory) опирается на
    // точное значение.
    LCD_CAM.lcd_clock.lcd_clkm_div_num = _cfg.bus_freq >= 20000000u ? 8 : 10;

    ESP_LOGI("S3", "Clock divider is %d", (int)LCD_CAM.lcd_clock.lcd_clkm_div_num);
    ESP_LOGD("S3", "Resulting output clock frequency: %d Mhz",  (int)(160000000L/LCD_CAM.lcd_clock.lcd_clkm_div_num)); 


    LCD_CAM.lcd_clock.lcd_clkm_div_a = 1;     // 0/1 fractional divide
    LCD_CAM.lcd_clock.lcd_clkm_div_b = 0;
    
    // See section 26.3.3.1 of the ESP32­S3 Technical Reference Manual
    // for information on other clock sources and dividers.

    // Configure LCD frame format. This is where we fiddle the peripheral
    // to provide generic 8-bit output rather than actually driving an LCD.
    // There's also a 16-bit mode but that's not shown here.
    LCD_CAM.lcd_ctrl.lcd_rgb_mode_en = 0;    // i8080 mode (not RGB)
    LCD_CAM.lcd_rgb_yuv.lcd_conv_bypass = 0; // Disable RGB/YUV converter
    LCD_CAM.lcd_misc.lcd_next_frame_en = 0;  // Do NOT auto-frame

    LCD_CAM.lcd_misc.lcd_bk_en = 1;          // https://esp32.com/viewtopic.php?t=24459&start=60#p91835
    
    LCD_CAM.lcd_data_dout_mode.val = 0;      // No data delays
    LCD_CAM.lcd_user.lcd_always_out_en = 1;  // Enable 'always out' mode
    LCD_CAM.lcd_user.lcd_8bits_order = 0;    // Do not swap bytes
    LCD_CAM.lcd_user.lcd_bit_order = 0;      // Do not reverse bit order
    LCD_CAM.lcd_user.lcd_2byte_en = 1;       // 8-bit data mode
    LCD_CAM.lcd_user.lcd_dummy = 1;          // Dummy phase(s) @ LCD start
    LCD_CAM.lcd_user.lcd_dummy_cyclelen = 1; // 1+1 dummy phase
    LCD_CAM.lcd_user.lcd_cmd = 0;            // No command at LCD start
    // "Dummy phases" are initial LCD peripheral clock cycles before data
    // begins transmitting when requested. After much testing, determined
    // that at least one dummy phase MUST be enabled for DMA to trigger
    // reliably. A problem with dummy phase(s) is if we're also using the
    // LCD_PCLK_IDX signal (not used in this code, but Adafruit_Protomatter
    // does)...the clock signal will start a couple of pulses before data,
    // which may or may not be problematic in some situations. You can
    // disable the dummy phase but need to keep the LCD TX FIFO primed
    // in that case, which gets complex.
    // always_out_en is set above to allow aribtrary-length transfers,
    // else lcd_dout_cyclelen is used...but is limited to 8K. Long (>4K)
    // transfers need DMA linked lists, not used here but mentioned later.

    // Route 8 LCD data signals to GPIO pins
    int8_t* pins = _cfg.pin_data;  

    for(int i = 0; i < 16; i++) 
    {
      if (pins[i] >= 0) { // -1 value will CRASH the ESP32!
        // Инверсия делается матрицей GPIO, а не в данных буфера: у LCD_CAM все
        // сигналы и так идут через неё, так что это ничего не стоит.
        esp_rom_gpio_connect_out_signal(pins[i], LCD_DATA_OUT0_IDX + i,
                                        (_cfg.invert_data_mask >> i) & 1u, false);
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)		
        // https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA/issues/803
		gpio_func_sel((gpio_num_t)pins[i], PIN_FUNC_GPIO);
#else
        gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[pins[i]], PIN_FUNC_GPIO);
#endif		
        gpio_set_drive_capability((gpio_num_t)pins[i], (gpio_drive_cap_t)3);    
      }
    }

    // Clock
      esp_rom_gpio_connect_out_signal(_cfg.pin_wr,  LCD_PCLK_IDX, _cfg.invert_pclk, false);
	  
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
	  // https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA/issues/803
	  gpio_func_sel((gpio_num_t)_cfg.pin_wr, PIN_FUNC_GPIO);
#else	  
      gpio_hal_iomux_func_sel(GPIO_PIN_MUX_REG[_cfg.pin_wr], PIN_FUNC_GPIO);
#endif  
      gpio_set_drive_capability((gpio_num_t)_cfg.pin_wr, (gpio_drive_cap_t)3);  


    _pclk_hz = kLcdSourceHz / (uint32_t)LCD_CAM.lcd_clock.lcd_clkm_div_num;

    // Allocate DMA channel and connect it to the LCD peripheral
    static gdma_channel_alloc_config_t dma_chan_config = {
      .sibling_chan = NULL,
      .direction = GDMA_CHANNEL_DIRECTION_TX,
      .flags = {
        .reserve_sibling = 0,
        // Обработчик извещения работает при отключённом кэше: без этого запись
        // во флеш (обновление прошивки, сохранение настроек) либо глушила бы
        // прерывание, либо роняла бы устройство на выборке кода из флеша.
        .isr_cache_safe = 1
      }
    };
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
    esp_err_t err = gdma_new_ahb_channel(&dma_chan_config, &dma_chan);
    if (err != ESP_OK) {
      ESP_LOGE("S3", "Failed to allocate AHB GDMA channel: %s", esp_err_to_name(err));
      return false;
    }
#else
    if (gdma_new_channel(&dma_chan_config, &dma_chan) != ESP_OK) return false;
#endif
    if (gdma_connect(dma_chan, GDMA_MAKE_TRIGGER(GDMA_TRIG_PERIPH_LCD, 0)) != ESP_OK) return false;
    static gdma_strategy_config_t strategy_config = {
      .owner_check = false,
      .auto_update_desc = false,
      .eof_till_data_popped = false
    };
    if (gdma_apply_strategy(dma_chan, &strategy_config) != ESP_OK) return false;

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 3, 0) // gdma_transfer_config_t introduced in IDF 5.3
    gdma_transfer_config_t transfer_config = {
#ifdef SPIRAM_DMA_BUFFER
      .max_data_burst_size = 64,
      .access_ext_mem = true
#else
      .max_data_burst_size = 32,
      .access_ext_mem = false
#endif
    };
    if (gdma_config_transfer(dma_chan, &transfer_config) != ESP_OK) return false;
#else
    gdma_transfer_ability_t ability = {
        .sram_trans_align = 32,
        .psram_trans_align = 64,
    };
    if (gdma_set_transfer_ability(dma_chan, &ability) != ESP_OK) return false;
#endif

    // Извещение о завершении marker нужно только двойной буферизации: на
    // одной копии обмена нет, и прерывание пять сотен раз в секунду было бы
    // платой ни за что.
    if (_double_dma_buffer)
    {
      _flip_wait_ticks = pdMS_TO_TICKS((hub75_flip::wait_timeout_us(_ring_bytes, _pclk_hz) + 999) / 1000);
      if (_flip_wait_ticks < 2)
      {
        // Меньше двух тиков ждать бессмысленно: первый истекает в любой момент
        // после вызова, вплоть до немедленного.
        _flip_wait_ticks = 2;
      }

      ESP_LOGI("S3", "Frame swap: ring %u bytes at %u Hz, wait limit %u ms.",
               (unsigned int)_ring_bytes, (unsigned int)_pclk_hz,
               (unsigned int)pdTICKS_TO_MS(_flip_wait_ticks));

      if (_eof_sem == nullptr)
      {
        _eof_sem = xSemaphoreCreateBinaryStatic(&_eof_sem_storage);
      }

      esp_err_t chan_err = gdma_get_channel_id(dma_chan, &_dma_chan_id);
      if (chan_err != ESP_OK)
      {
        ESP_LOGE("S3", "Failed to read the GDMA channel number: %s", esp_err_to_name(chan_err));
        return false;
      }
      else
      {
        static gdma_tx_event_callbacks_t tx_cbs = {
          // .on_trans_eof is literally the only gdma tx event type available
          .on_trans_eof = Bus_Parallel16::eof_isr,
          .on_descr_err = NULL
        };
        chan_err = gdma_register_tx_event_callbacks(dma_chan, &tx_cbs, this);
        if (chan_err != ESP_OK)
        {
          ESP_LOGE("S3", "Failed to register the GDMA end-of-pass callback: %s", esp_err_to_name(chan_err));
          _dma_chan_id = -1;
          return false;
        }
      }
    }

    // This uses a busy loop to wait for each DMA transfer to complete...
    // but the whole point of DMA is that one's code can do other work in
    // the interim. The CPU is totally free while the transfer runs!
    while (LCD_CAM.lcd_user.lcd_start); // Wait for DMA completion callback

    // After much experimentation, each of these steps is required to get
    // a clean start on the next LCD transfer:
    if (gdma_reset(dma_chan) != ESP_OK) return false;
    esp_rom_delay_us(1000);
     
    LCD_CAM.lcd_user.lcd_dout        = 1; // Enable data out
    LCD_CAM.lcd_user.lcd_update      = 1; // Update registers
    LCD_CAM.lcd_misc.lcd_afifo_reset = 1; // Reset LCD TX FIFO


    return true; // no return val = illegal instruction
 }


  void Bus_Parallel16::release(void)
  {
    // Канал GDMA освобождается вместе с остальным: без этого каждая
    // перенастройка панели забирала по каналу из пяти, и после нескольких
    // смен настроек панель уже не поднималась.
    _dma_running = false;
    if (_peripheral_enabled) {
      LCD_CAM.lcd_user.lcd_start = 0;
      LCD_CAM.lcd_user.lcd_reset = 1;
      LCD_CAM.lcd_user.lcd_update = 1;
    }
    if (dma_chan)
    {
      gdma_stop(dma_chan);
      gdma_disconnect(dma_chan);
      gdma_del_channel(dma_chan);
      dma_chan = nullptr;
    }
    // Только после того, как канал удалён: пока он жив, обработчик прерывания
    // может дотянуться до семафора.
    _dma_chan_id = -1;
    if (_eof_sem)
    {
      vSemaphoreDelete(_eof_sem);
      _eof_sem = nullptr;
    }
    if (_i80_bus)
    {
      esp_lcd_del_i80_bus(_i80_bus);
      _i80_bus = nullptr;
    }
    // The panel owner frees descriptors after this release has removed GDMA
    // and its ISR. Never free a reachable descriptor from the interrupt.
    _dmadesc_a = nullptr;
    _dmadesc_b = nullptr;
    _dmadesc_count = 0;
    if (_peripheral_enabled) {
      periph_module_disable(PERIPH_LCD_CAM_MODULE);
      _peripheral_enabled = false;
    }
  }

  void Bus_Parallel16::enable_double_dma_desc(void)
  {
        ESP_LOGI("S3", "Enabled support for secondary DMA buffer.");    
       _double_dma_buffer = true;
  }

  // Need this to work for double buffers etc.
  bool Bus_Parallel16::allocate_dma_desc_memory(size_t len)
  {
    // Each descriptor ring is contiguous, independently of pixel payloads.
    // Both rings are tracked by the same generation owner as the row buffers.
    _dmadesc_count = len;
    _ring_bytes = 0;
    const size_t bytes = sizeof(HUB75_DMA_DESCRIPTOR_T) * len;

    ESP_LOGD("S3", "Taking %d bytes of the DMA pool for descriptors.", (int)bytes);

    _dmadesc_a = (HUB75_DMA_DESCRIPTOR_T*)ledcast::dma_pool_allocate(bytes);

    if (_dmadesc_a == nullptr)
    {
      ESP_LOGE("S3", "ERROR: %u bytes for _dmadesc_a do not fit the DMA pool (%u of %u bytes free).",
               (unsigned int)bytes,
               (unsigned int)(ledcast::dma_pool_capacity() - ledcast::dma_pool_used()),
               (unsigned int)ledcast::dma_pool_capacity());
      _dmadesc_count = 0;
      return false;
    }

    if (_double_dma_buffer)
    {
      _dmadesc_b = (HUB75_DMA_DESCRIPTOR_T*)ledcast::dma_pool_allocate(bytes);

      if (_dmadesc_b == nullptr)
      {
        // Silently falling back to a single descriptor chain would leave
        // flip_dma_output_buffer() dereferencing a null _dmadesc_b on the first
        // flip. Fail the setup instead, so the caller can report it.
        ESP_LOGE("S3", "ERROR: %u bytes for _dmadesc_b do not fit the DMA pool.", (unsigned int)bytes);
        _dmadesc_a = nullptr;
        _dmadesc_count = 0;
        return false;
      }
    }

    /// override static 
    _dmadesc_a_idx = 0;
    _dmadesc_b_idx = 0;

    return true;

  }

  void Bus_Parallel16::create_dma_desc_link(void *data, size_t size, bool dmadesc_b)
  {
    static constexpr size_t MAX_DMA_LEN = (4096-4);

    if (size > MAX_DMA_LEN) {
      size = MAX_DMA_LEN;
      ESP_LOGW("S3", "Creating DMA descriptor which links to payload with size greater than MAX_DMA_LEN!");            
    }

    if ( dmadesc_b == true)
    {
      if (_dmadesc_b == nullptr || _dmadesc_b_idx >= _dmadesc_count)
      {
        ESP_LOGE("S3", "Secondary DMA descriptor index exceeds allocated ring.");
        return;
      }
      // The pool is reused after a display reconfiguration and is not zeroed.
      // Initialize the entire descriptor, including reserved/status bits,
      // before publishing it to GDMA.
      _dmadesc_b[_dmadesc_b_idx] = {};
      _dmadesc_b[_dmadesc_b_idx].dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
      //_dmadesc_b[_dmadesc_b_idx].dw0.suc_eof = 0;   
      _dmadesc_b[_dmadesc_b_idx].dw0.suc_eof = (_dmadesc_b_idx == 0);
      _dmadesc_b[_dmadesc_b_idx].dw0.size = _dmadesc_b[_dmadesc_b_idx].dw0.length = size; //sizeof(data);
      _dmadesc_b[_dmadesc_b_idx].buffer = data; //data;

      if (_dmadesc_b_idx == _dmadesc_count-1) {
          _dmadesc_b[_dmadesc_b_idx].next =  (dma_descriptor_t *) &_dmadesc_b[0]; 
      }
      else {
          _dmadesc_b[_dmadesc_b_idx].next =  (dma_descriptor_t *) &_dmadesc_b[_dmadesc_b_idx+1]; 
      }

      _dmadesc_b_idx++;


    }
    else
    {
        
      if ( _dmadesc_a_idx >= _dmadesc_count)
      {
        ESP_LOGE("S3", "Attempted to create more DMA descriptors than allocated. Expecting max %u descriptors.", (unsigned int)_dmadesc_count);          
        return;
      }

      _dmadesc_a[_dmadesc_a_idx] = {};
      _dmadesc_a[_dmadesc_a_idx].dw0.owner = DMA_DESCRIPTOR_BUFFER_OWNER_DMA;
      //_dmadesc_a[_dmadesc_a_idx].dw0.suc_eof = 0;
      _dmadesc_a[_dmadesc_a_idx].dw0.suc_eof = (_dmadesc_a_idx == 0);
      _dmadesc_a[_dmadesc_a_idx].dw0.size = _dmadesc_a[_dmadesc_a_idx].dw0.length = size; //sizeof(data);
      _dmadesc_a[_dmadesc_a_idx].buffer = data; //data;

      // Длина кольца в байтах — это и есть длительность прохода, выраженная в
      // геометрии панели: ширина цепочки, адресные строки, разряды и повторы
      // старших планов уже сложены в эти дескрипторы. Обе цепочки одинаковы,
      // поэтому считается одна.
      _ring_bytes += (uint32_t)size;

      if (_dmadesc_a_idx == _dmadesc_count-1) {
          _dmadesc_a[_dmadesc_a_idx].next =  (dma_descriptor_t *) &_dmadesc_a[0]; 
			ESP_LOGV("S3", "Creating last _dmadesc_a descriptor which loops back to _dmadesc_a[0]!");     		  
      }
      else {
          _dmadesc_a[_dmadesc_a_idx].next =  (dma_descriptor_t *) &_dmadesc_a[_dmadesc_a_idx+1]; 
      }

      _dmadesc_a_idx++;


    }

  } // end create_dma_desc_link

  bool Bus_Parallel16::dma_transfer_start()
  {
    if (!dma_chan || !_dmadesc_a || !_dmadesc_count) return false;
    _pending_marker_target = -1;
    __atomic_store_n(&_completed_markers[0], 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&_completed_markers[1], 0u, __ATOMIC_RELEASE);
    if (gdma_start(dma_chan, (intptr_t)&_dmadesc_a[0]) != ESP_OK) return false;
    esp_rom_delay_us(100);              // Must 'bake' a moment before...
    LCD_CAM.lcd_user.lcd_start = 1;        // Trigger LCD DMA transfer

    _dma_running = true;
    return true;

  } // end

  void Bus_Parallel16::dma_transfer_stop()
  {

        _dma_running = false;

        LCD_CAM.lcd_user.lcd_reset = 1;        // Trigger LCD DMA transfer
        LCD_CAM.lcd_user.lcd_update = 1;        // Trigger LCD DMA transfer

        if (dma_chan) gdma_stop(dma_chan);
        _pending_marker_target = -1;

  } // end


  uint32_t Bus_Parallel16::pass_period_us() const
  {
    return hub75_flip::ring_period_us(_ring_bytes, _pclk_hz);
  }

  esp_err_t Bus_Parallel16::flip_dma_output_buffer(int back_buffer_id)
  {
    if (!_double_dma_buffer || _dmadesc_a == nullptr || _dmadesc_b == nullptr || _dmadesc_count == 0 ||
        (back_buffer_id != 0 && back_buffer_id != 1))
    {
      return ESP_ERR_INVALID_STATE;
    }

    HUB75_DMA_DESCRIPTOR_T* target = (back_buffer_id == 1) ? _dmadesc_b : _dmadesc_a;

    if (_dma_running && (_eof_sem == nullptr || _dma_chan_id < 0))
      return ESP_ERR_INVALID_STATE;

    if (_pending_marker_target >= 0 && _pending_marker_target != back_buffer_id)
      return ESP_ERR_INVALID_STATE;

    if (_pending_marker_target < 0)
    {
      _pending_marker_baseline = __atomic_load_n(&_completed_markers[back_buffer_id], __ATOMIC_ACQUIRE);
      _pending_marker_target = back_buffer_id;
    }

    // Отметки прошлых проходов кольца к этому обмену отношения не имеют.
    // Снимаются до записи ссылок, а не после: отметка, поставленная между
    // снятием и записью, только заставит лишний раз перечитать состояние DMA.
    if (_eof_sem != nullptr)
    {
      xSemaphoreTake(_eof_sem, 0);
    }

    // All RGB bitplane writes must become visible before DMA can observe the
    // descriptor link to the new frame. On S3 both live in internal SRAM, but
    // the CPU is otherwise free to reorder ordinary stores around these stores.
    __sync_synchronize();
    _dmadesc_b[_dmadesc_count-1].next = (dma_descriptor_t *) target;  // setup loop
    _dmadesc_a[_dmadesc_count-1].next = (dma_descriptor_t *) target;  // flip across
    __sync_synchronize();

    // Вывод стоит (панель ещё не запущена или уже остановлена) — ждать нечего:
    // цепочка никого не показывает, и старая копия свободна и так.
    if (!_dma_running)
    {
      _pending_marker_target = -1;
      return ESP_OK;
    }

    // A successful preceding flip consumed an EOF from the other ring before
    // that ring was made writable. Thus an old target EOF cannot remain in
    // flight across that handover. Require a NEW target marker after arming;
    // stale semaphore tokens and old counter values never grant ownership.
    // Preserve the baseline on timeout: completion between retries must not
    // be forgotten, and both framebuffers remain reserved until it arrives.
    const TickType_t wait_started = xTaskGetTickCount();
    while (__atomic_load_n(&_completed_markers[back_buffer_id], __ATOMIC_ACQUIRE) ==
           _pending_marker_baseline)
    {
      // Old-ring EOFs can keep arriving if a descriptor handover fails. They
      // must not restart the timeout indefinitely.
      const TickType_t elapsed = xTaskGetTickCount() - wait_started;
      if (elapsed >= _flip_wait_ticks ||
          xSemaphoreTake(_eof_sem, _flip_wait_ticks - elapsed) != pdTRUE)
      {
        // The target may have become current concurrently with the timeout
        // while its EOF notification was coalesced or delayed. Re-read the
        // hardware before reporting an unresolved transition.
        if (__atomic_load_n(&_completed_markers[back_buffer_id], __ATOMIC_ACQUIRE) !=
            _pending_marker_baseline)
        {
          _pending_marker_target = -1;
          return ESP_OK;
        }
        // Путь отказа: извещения нет дольше трёх проходов кольца. Управление
        // возвращается вызывающей стороне, и она сама решит, что делать; висеть
        // здесь нельзя — кадр закрывают и рендер, и приём по сети.
        return ESP_ERR_TIMEOUT;
      }
    }

    _pending_marker_target = -1;
    return ESP_OK;

  } // end flip


#endif
