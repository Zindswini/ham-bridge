#include <stdio.h>
#include <inttypes.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "driver/i2c_master.h"
#include "esp_flash.h"
#include "esp_system.h"
#include "esp_log.h"

// U8G2 OLED Graphics Library
#include "u8g2.h"
#include "u8g2_esp32_hal.h"

// Other ham_bridge components
#include "i2s_handler.h"

// OLED Display and ES8388 Control
#define PIN_SDA 1
#define PIN_SCL 15

static const char* TAG = "MAIN";

void drawScreen(u8g2_t *u8g2) {
    u8g2_ClearBuffer(u8g2);

    u8g2_SetFont(u8g2, u8g2_font_helvB08_tr);
    u8g2_DrawButtonUTF8(u8g2, 64, 24, U8G2_BTN_SHADOW1|U8G2_BTN_HCENTER|U8G2_BTN_BW2, 56, 2, 2, "Testing 12 :3");
    
    int current_timestamp = pdTICKS_TO_MS(xTaskGetTickCount());
    char timestamp_string[10];
    snprintf(timestamp_string, 9, "%d", current_timestamp);

    u8g2_DrawButtonUTF8(u8g2, 64, 50, U8G2_BTN_SHADOW1|U8G2_BTN_HCENTER|U8G2_BTN_BW2, 56, 2, 2, timestamp_string);

    u8g2_SendBuffer(u8g2);
}

void app_main(void)
{
    vTaskDelay(pdMS_TO_TICKS(1500)); // Delay for monitoring reconnect
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

    // Init I2C Master Bus
    ESP_LOGI(TAG, "Initializing I2C Bus");
    i2c_master_bus_handle_t i2c_bus_handle = NULL;
    i2c_master_bus_config_t i2c_mst_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_mst_cfg, &i2c_bus_handle));
    i2c_master_bus_reset(i2c_bus_handle);
    ESP_LOGI(TAG, "Initialized I2C Bus at address %p", (void*)i2c_bus_handle);

    ESP_LOGI(TAG, "Configuring U8G2 HAL Object");
    u8g2_esp32_hal_t u8g2_esp32_hal = U8G2_ESP32_HAL_DEFAULT;
    u8g2_esp32_hal.bus.i2c.i2c_bus_handle = i2c_bus_handle;
    u8g2_esp32_hal_init(u8g2_esp32_hal);

    u8g2_t u8g2; // Structure for holding display state

    u8g2_Setup_ssd1306_i2c_128x64_noname_f(&u8g2, U8G2_R0, u8g2_esp32_i2c_byte_cb, u8g2_esp32_gpio_and_delay_cb); // I2C Callback functions for mapping
    
    ESP_LOGI(TAG, "Preparing to Initialize U8G2");
    u8x8_SetI2CAddress(&u8g2.u8x8, 0x78);
    u8g2_InitDisplay(&u8g2);
    u8g2_SetPowerSave(&u8g2, 0);
    ESP_LOGI(TAG, "Initialized U8G2 and Display");

    esp_log_level_set("*", ESP_LOG_INFO);
    drawScreen(&u8g2);
    esp_log_level_set("*", ESP_LOG_DEBUG);

    ESP_LOGI(TAG, "Initializing I2S Driver");
    i2s_driver_init();
    ESP_LOGI(TAG, "Initialized I2S Driver");

    ESP_LOGI(TAG, "Initializing I2S Codec");
    es8388_codec_init(i2c_bus_handle);
    ESP_LOGI(TAG, "Initialized I2S Codec");

    esp_log_level_set("*", ESP_LOG_INFO);
    drawScreen(&u8g2);

    ESP_LOGE(TAG, "Starting music task");
    xTaskCreate(i2s_music, "i2s_music", 4096, NULL, 5, NULL);
    ESP_LOGE(TAG, "Created music task");

    vTaskDelay(pdMS_TO_TICKS(60000));
    
    while(true)
    {
        esp_log_level_set("*", ESP_LOG_INFO);
        drawScreen(&u8g2);
        vTaskDelay(100);
    }
}


