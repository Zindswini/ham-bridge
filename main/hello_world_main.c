/*
 * SPDX-FileCopyrightText: 2010-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: CC0-1.0
 */

#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_log.h"

// U8G2 OLED Graphics Library
#include "u8g2.h"
#include "u8g2_esp32_hal.h"

// Espressif I2S Codec Library
#include "esp_codec_dev.h"

// OLED Display and ES8388 Control
#define PIN_SDA 0
#define PIN_SCL 1

// ES8388 I2S Pins
#define PIN_DOUT  19
#define PIN_LRCLK 18
#define PIN_DIN   21
#define PIN_SCLK  2
#define PIN_MCLK  3

static const char* TAG = "MAIN";

void app_main(void)
{
    ESP_LOGI(TAG, "Hello world!");

    /* Print chip information */
    esp_chip_info_t chip_info;
    uint32_t flash_size;
    esp_chip_info(&chip_info);
    ESP_LOGI(TAG, "This is %s chip with %d CPU core(s), %s%s%s%s, ",
           CONFIG_IDF_TARGET,
           chip_info.cores,
           (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi/" : "",
           (chip_info.features & CHIP_FEATURE_BT) ? "BT" : "", // :3
           (chip_info.features & CHIP_FEATURE_BLE) ? "BLE" : "",
           (chip_info.features & CHIP_FEATURE_IEEE802154) ? ", 802.15.4 (Zigbee/Thread)" : "");

    unsigned major_rev = chip_info.revision / 100;
    unsigned minor_rev = chip_info.revision % 100;
    ESP_LOGI(TAG, "silicon revision v%d.%d, ", major_rev, minor_rev);
    if(esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        ESP_LOGI(TAG, "Get flash size failed");
        return;
    }

    ESP_LOGI(TAG, "%" PRIu32 "MB %s flash", flash_size / (uint32_t)(1024 * 1024),
           (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded" : "external");

    ESP_LOGI(TAG, "Minimum free heap size: %" PRIu32 " bytes", esp_get_minimum_free_heap_size());

    ESP_LOGI(TAG, "Creating LCD HAL");
    u8g2_esp32_hal_t u8g2_esp32_hal = U8G2_ESP32_HAL_DEFAULT;
    u8g2_esp32_hal.bus.i2c.sda = PIN_SDA;
    u8g2_esp32_hal.bus.i2c.scl = PIN_SCL;
    u8g2_esp32_hal_init(u8g2_esp32_hal);

    u8g2_t u8g2; // Structure for holding display state

    u8g2_Setup_ssd1306_i2c_128x64_noname_f(&u8g2, U8G2_R0, u8g2_esp32_i2c_byte_cb, u8g2_esp32_gpio_and_delay_cb); // I2C Callback functions for mapping
    
    ESP_LOGI(TAG, "Pre-Initialization");
    u8x8_SetI2CAddress(&u8g2.u8x8, 0x78);
    u8g2_InitDisplay(&u8g2);
    ESP_LOGI(TAG, "Initialized display");
    
    u8g2_SetPowerSave(&u8g2, 0);
    
    while(true)
    {
        ESP_LOGI(TAG, "Alive!");
        u8g2_ClearBuffer(&u8g2);

        u8g2_SetFont(&u8g2, u8g2_font_helvB08_tr);
        u8g2_DrawButtonUTF8(&u8g2, 64, 50, U8G2_BTN_SHADOW1|U8G2_BTN_HCENTER|U8G2_BTN_BW2, 56, 2, 2, "Testing 12 :3");

        u8g2_SendBuffer(&u8g2);   
        vTaskDelay(10);
    }
}
