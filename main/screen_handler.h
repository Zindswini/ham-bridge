#ifndef SCREEN_HANDLER_H
#define SCREEN_HANDLER_H

#include "u8g2.h"
#include "u8g2_esp32_hal.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"


void drawScreen(u8g2_t *u8g2);

void initializeU8G2(u8g2_t *u8g2, i2c_master_bus_handle_t *i2c_bus_handle);

#endif