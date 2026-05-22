#ifndef I2S_HANDLER_H
#define I2S_HANDLER_H

#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "esp_log.h"
#include "driver/i2c_types.h"

// Espressif I2S Codec Library
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "driver/i2s_std.h"

esp_err_t i2s_driver_init(void);
esp_err_t es8388_codec_init(i2c_master_bus_handle_t i2c_bus_handle);

#endif