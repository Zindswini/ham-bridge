#ifndef I2S_HANDLER_H
#define I2S_HANDLER_H

#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "driver/i2c_types.h"
#include "config.h"

// Espressif I2S Codec Library
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "driver/i2s_std.h"

extern const uint8_t music_pcm_start[] asm("_binary_rick_pcm_start");
extern const uint8_t music_pcm_end[] asm("_binary_rick_pcm_end");

esp_err_t i2s_driver_init(void);
esp_err_t es8388_codec_init(i2c_master_bus_handle_t i2c_bus_handle);
void i2s_music(void *args);

#endif