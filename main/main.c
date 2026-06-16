#include "driver/i2c_master.h"
#include "esp_chip_info.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_system.h"
#include <freertos/FreeRTOS.h>
#include <freertos/projdefs.h>
#include <freertos/task.h>
#include <inttypes.h>
#include <stdlib.h>
#include <time.h>

// Other ham_bridge components
#include "config.h"
#include "ethernet_handler.h"
#include "http_server.h"
#include "i2s_handler.h"
#include "input_handler.h"
#include "nvs_flash.h"
#include "screen_handler.h"

static const char *TAG = "MAIN";

i2c_master_bus_handle_t i2c_bus_handle;
uint32_t last_draw_time;

void debugOutputTask(void) {}

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

  // Initialize Drivers and Objects

  ESP_LOGI(TAG, "Setting Up Button GPIO and Timer");
  setupButtonGPIOTimer();
  ESP_LOGI(TAG, "Set Up Button GPIO and Timer");

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

  ESP_LOGI(TAG, "Initializing U8G2 Display Object");
  initializeU8G2(&i2c_bus_handle);
  ESP_LOGI(TAG, "Initialized U8G2 Display Structure");

  ESP_LOGI(TAG, "Initializing NVS Flash");
  drawLoadingScreen("Initializing NVS Flash");
  ESP_ERROR_CHECK(nvs_flash_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  ESP_LOGI(TAG, "Initialized NVS Flash");

  ESP_LOGI(TAG, "Initializing I2S Driver");
  drawLoadingScreen("Initializing I2S Driver");
  i2sDriverInit();
  ESP_LOGI(TAG, "Initialized I2S Driver");

  ESP_LOGI(TAG, "Initializing I2S Codec");
  drawLoadingScreen("Initializing I2S Codec");
  es8388CodecInit(i2c_bus_handle);
  ESP_LOGI(TAG, "Initialized I2S Codec");

  ESP_LOGI(TAG, "Initializing Ethernet Driver");
  drawLoadingScreen("Initializing Eth Driver");
  setupEthernet();
  ESP_LOGI(TAG, "Initialized Ethernet Driver");

  // Set timezone
  ESP_LOGI(TAG, "Configuring NTP & TZ");
  drawLoadingScreen("Configuring NTP");
  esp_sntp_config_t sntp_config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
  esp_netif_sntp_init(&sntp_config);
  setenv("TZ", "UTC", 1);
  tzset();
  ESP_LOGI(TAG, "Configured NTP and Time Zone");

  // Start FreeRTOS Tasks
  drawLoadingScreen("Starting Tasks");
  ESP_LOGI(TAG, "Starting music task");
  xTaskCreate((TaskFunction_t)playI2sMusic, "i2s_music", 4096, NULL, 3,
              nullptr);
  ESP_LOGI(TAG, "Created music task");

  ESP_LOGI(TAG, "Starting Input Handler Task");
  xTaskCreate((TaskFunction_t)processInputsTask, "process_inputs", 4096, NULL,
              5, nullptr);
  ESP_LOGI(TAG, "Created Input Handler Task");

  ESP_LOGI(TAG, "Starting Screen Refresh Task");
  xTaskCreate((TaskFunction_t)screenRefreshTask, "screen_refresh", 4096, NULL,
              8, nullptr);
  ESP_LOGI(TAG, "Created Screen Refresh Task");

  ESP_LOGI(TAG, "Starting Web Server Task");
  xTaskCreate((TaskFunction_t)wssServerTask, "wss_web_server", 4096, NULL, 8,
              nullptr);
  ESP_LOGI(TAG, "Created Web Server Task");

  while (1) {
    char *outBuf = nullptr;
    outBuf = malloc(sizeof(char) * 2048);
    vTaskList(outBuf);
    ESP_LOGI(TAG, outBuf);
    free(outBuf);
    vTaskDelay(10000);
  }
}
