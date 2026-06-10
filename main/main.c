#include "driver/i2c_master.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include <inttypes.h>

// U8G2 OLED Graphics Library
#include "u8g2.h"

// Other ham_bridge components
#include "config.h"
#include "i2s_handler.h"
#include "input_handler.h"
#include "screen_handler.h"

static const char *TAG = "MAIN";

i2c_master_bus_handle_t i2c_bus_handle;
u8g2_t u8g2;
uint32_t last_draw_time;

void app_main(void) {
  vTaskDelay(pdMS_TO_TICKS(1500)); // Delay for monitoring reconnect
  ESP_LOGI(TAG, "Hello world!");

  /* Print chip information */
  esp_chip_info_t chip_info;
  uint32_t flash_size = 0;
  esp_chip_info(&chip_info);
  ESP_LOGI(TAG, "This is %s chip with %d CPU core(s), %s%s%s%s, ",
           CONFIG_IDF_TARGET, chip_info.cores,
           (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi/" : "",
           (chip_info.features & CHIP_FEATURE_BT) ? "BT" : "", // :3
           (chip_info.features & CHIP_FEATURE_BLE) ? "BLE" : "",
           (chip_info.features & CHIP_FEATURE_IEEE802154)
               ? ", 802.15.4 (Zigbee/Thread)"
               : "");

  unsigned major_rev = chip_info.revision / 100;
  unsigned minor_rev = chip_info.revision % 100;
  ESP_LOGI(TAG, "silicon revision v%d.%d, ", major_rev, minor_rev);
  if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
    ESP_LOGI(TAG, "Get flash size failed");
    return;
  }

  ESP_LOGI(TAG, "%" PRIu32 "MB %s flash", flash_size / (uint32_t)(1024 * 1024),
           (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded"
                                                         : "external");

  ESP_LOGI(TAG, "Minimum free heap size: %" PRIu32 " bytes",
           esp_get_minimum_free_heap_size());

  ESP_LOGI(TAG, "Setting Up GPIO Callbacks");
  setupGpio();
  ESP_LOGI(TAG, "Set Up GPIO Callbacks");

  // Init I2C Master Bus
  ESP_LOGI(TAG, "Initializing I2C Bus");

  i2c_master_bus_config_t i2c_mst_cfg = {
      .i2c_port = I2C_NUM_0,
      .sda_io_num = PIN_SDA,
      .scl_io_num = PIN_SCL,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = 1,
  };
  ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_mst_cfg, &i2c_bus_handle));
  i2c_master_bus_reset(i2c_bus_handle);
  ESP_LOGI(TAG, "Initialized I2C Bus at address %p", (void *)i2c_bus_handle);

  initializeU8G2(&u8g2, &i2c_bus_handle);

  ESP_LOGI(TAG, "Initializing I2S Driver");
  i2sDriverInit();
  ESP_LOGI(TAG, "Initialized I2S Driver");

  ESP_LOGI(TAG, "Initializing I2S Codec");
  es8388CodecInit(i2c_bus_handle);
  ESP_LOGI(TAG, "Initialized I2S Codec");

  ESP_LOGI(TAG, "Starting music task");
  xTaskCreate((TaskFunction_t)playI2sMusic, "i2s_music", 4096, NULL, 5,
              nullptr);
  ESP_LOGI(TAG, "Created music task");

  ESP_LOGI(TAG, "Starting Input Handler Task");
  xTaskCreate((TaskFunction_t)processInputs, "process_inputs", 4096, NULL, 5,
              nullptr);
  ESP_LOGI(TAG, "Created Input Handler Task");

  while (true) {
    drawScreen(&u8g2);
    vTaskDelay(100);
  }
}
