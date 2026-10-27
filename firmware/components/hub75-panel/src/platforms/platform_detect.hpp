/*----------------------------------------------------------------------------/
Original Source:
 https://github.com/lovyan03/LovyanGFX/

Licence:
 [FreeBSD](https://github.com/lovyan03/LovyanGFX/blob/master/license.txt)

Author:
 [lovyan03](https://twitter.com/lovyan03)

Contributors:
 [ciniml](https://github.com/ciniml)
 [mongonta0716](https://github.com/mongonta0716)
 [tobozo](https://github.com/tobozo)

Modified heavily for the ESP32 HUB75 DMA library by:
 [mrfaptastic](https://github.com/mrfaptastic)
/----------------------------------------------------------------------------*/
#pragma once

// Прибор собран на ESP32-S3, и вывод у него идёт через LCD_CAM с GDMA. Ветки
// под остальные кристаллы из форка убраны: они выводили через I2S, у них своя
// работа с памятью и своя сигнатура обмена копиями кадра, и ни собрать, ни
// проверить их на этом приборе нечем.

#if defined (ESP_PLATFORM)

 #include <sdkconfig.h>

 #if defined (CONFIG_IDF_TARGET_ESP32S3)

  #include "esp32s3/gdma_lcd_parallel16.hpp"
  #include "esp32s3/esp32s3-default-pins.hpp"

  #if defined(SPIRAM_FRAMEBUFFER)
	#pragma message "Use SPIRAM_DMA_BUFFER instead."
  #endif

  #if defined(SPIRAM_DMA_BUFFER)
   #pragma message "Enabling use of PSRAM/SPIRAM based DMA Buffer"

   // Disable fast functions because I don't understand the interaction with DMA PSRAM and the CPU->DMA->SPIRAM Cache implications..
   #define NO_FAST_FUNCTIONS 1

  #endif

 #else

  #error "This library builds for the ESP32-S3 only."

 #endif

#endif
