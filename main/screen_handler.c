#include "screen_handler.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "u8g2.h"
#include "u8g2_esp32_hal.h"

static const char *TAG = "SCREEN_HANDLER";

void drawScreen(u8g2_t *u8g2) {
  u8g2_ClearBuffer(u8g2);

  u8g2_SetFont(u8g2, u8g2_font_helvB08_tr);
  u8g2_DrawButtonUTF8(u8g2, 64, 24,
                      U8G2_BTN_SHADOW1 | U8G2_BTN_HCENTER | U8G2_BTN_BW2, 56, 2,
                      2, "Testing 12 :3");

  int current_timestamp = pdTICKS_TO_MS(xTaskGetTickCount());
  char timestamp_string[10];
  snprintf(timestamp_string, 9, "%d", current_timestamp);

  u8g2_DrawButtonUTF8(u8g2, 64, 50,
                      U8G2_BTN_SHADOW1 | U8G2_BTN_HCENTER | U8G2_BTN_BW2, 56, 2,
                      2, timestamp_string);

  u8g2_SendBuffer(u8g2);
}

void initializeU8G2(u8g2_t *u8g2, i2c_master_bus_handle_t *i2c_bus_handle) {
  ESP_LOGI(TAG, "Configuring U8G2 HAL Object");
  u8g2_esp32_hal_t u8g2_esp32_hal = U8G2_ESP32_HAL_DEFAULT;
  u8g2_esp32_hal.bus.i2c.i2c_bus_handle = *i2c_bus_handle;
  u8g2_esp32_hal_init(u8g2_esp32_hal);

  u8g2_Setup_ssd1306_i2c_128x64_noname_f(
      u8g2, U8G2_R0, u8g2_esp32_i2c_byte_cb,
      u8g2_esp32_gpio_and_delay_cb); // I2C Callback functions for mapping

  ESP_LOGI(TAG, "Preparing to Initialize U8G2");
  u8x8_SetI2CAddress(&(u8g2->u8x8), 0x78);
  u8g2_InitDisplay(u8g2);
  u8g2_SetPowerSave(u8g2, 0);
  ESP_LOGI(TAG, "Initialized U8G2 and Display");
}