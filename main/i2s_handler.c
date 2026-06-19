#include "i2s_handler.h"
#include "config.h"

#include "driver/i2c_types.h"
#include "esp_log.h"
#include <freertos/FreeRTOS.h>
#include <stdio.h>
#include <sys/cdefs.h>

// Espressif I2S Codec Library
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

// I2S Handles
static i2s_chan_handle_t tx_handle = nullptr;
static i2s_chan_handle_t rx_handle = nullptr;

static const char *TAG = "I2S_HANDLER";

esp_err_t i2sDriverInit(void) {
  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true;
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle));

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = PIN_MCLK,
              .bclk = PIN_SCLK,
              .ws = PIN_LRCLK,
              .din = PIN_DIN,
              .dout = PIN_DOUT,
              .invert_flags =
                  {
                      .mclk_inv = 0,
                      .bclk_inv = 0,
                      .ws_inv = 0,
                  },
          },
  };
  std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULITPLE;

  ESP_LOGD(TAG, "Initializing I2S Channels");
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
  // ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
  ESP_LOGD(TAG, "Initialized I2S Channels");

  ESP_LOGD(TAG, "Enabling I2S Channels");
  ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
  // ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
  ESP_LOGD(TAG, "Enabled I2S Channels");
  return ESP_OK;
}

esp_err_t es8388CodecInit(i2c_master_bus_handle_t i2c_bus_handle) {
  // Create I2S control interface with I2C bus handle
  ESP_LOGD(TAG, "Initializing I2S I2C Config");
  audio_codec_i2c_cfg_t i2s_i2c_cfg = {
      .port = I2C_NUM_0,
      .addr = ES8388_CODEC_DEFAULT_ADDR,
      .bus_handle = i2c_bus_handle,
  };
  const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2s_i2c_cfg);
  assert(ctrl_if);
  ESP_LOGD(TAG, "Initialized I2S I2C Config");

  ESP_LOGD(TAG, "Initializing I2S Config");
  audio_codec_i2s_cfg_t i2s_cfg = {
      .port = I2S_NUM_0,
      .rx_handle = rx_handle,
      .tx_handle = tx_handle,
  };
  const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
  assert(data_if);
  ESP_LOGD(TAG, "Initialized I2S Config");

  ESP_LOGD(TAG, "Initializing I2S GPIO Config");
  const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
  assert(gpio_if);
  es8388_codec_cfg_t es8388_cfg = {
      .ctrl_if = ctrl_if,
      .gpio_if = gpio_if,
      .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
      .master_mode = false,
      .pa_pin = PIN_PA,
      .pa_reverted = false,
      .hw_gain =
          {
              .pa_voltage = VCC_VOLTAGE,
              .codec_dac_voltage = VCC_VOLTAGE,
              .pa_gain = 0,
          },
  };
  const audio_codec_if_t *es8388_if = es8388_codec_new(&es8388_cfg);
  assert(es8388_if);
  ESP_LOGD(TAG, "Initialized I2S GPIO Config");

  ESP_LOGD(TAG, "Initializing I2S Device Config");
  esp_codec_dev_cfg_t dev_cfg = {
      .dev_type = ESP_CODEC_DEV_TYPE_OUT,
      .codec_if = es8388_if,
      .data_if = data_if,
  };
  esp_codec_dev_handle_t codec_handle = esp_codec_dev_new(&dev_cfg);
  assert(codec_handle);
  ESP_LOGD(TAG, "Initialized I2S Device Config");

  ESP_LOGD(TAG, "Initializing I2S Sample Config");
  esp_codec_dev_sample_info_t sample_cfg = {.bits_per_sample =
                                                I2S_DATA_BIT_WIDTH_16BIT,
                                            .channel = 2,
                                            .channel_mask = 0x03,
                                            .sample_rate = I2S_SAMPLE_RATE,
                                            .mclk_multiple = I2S_MCLK_MULITPLE};

  if (esp_codec_dev_open(codec_handle, &sample_cfg) != ESP_CODEC_DEV_OK) {
    ESP_LOGE(TAG, "Open codec device failed");
    return ESP_FAIL;
  }

  if (esp_codec_dev_set_out_vol(codec_handle, 100) != ESP_CODEC_DEV_OK) {
    ESP_LOGE(TAG, "set output volume failed");
    return ESP_FAIL;
  }
  ESP_LOGD(TAG, "Initialized I2S Sample Config");
  return ESP_OK;
}

void playI2sMusic(void *args __unused) {
  esp_err_t status = ESP_OK;
  size_t bytes_written = 0;
  uint8_t *data_ptr = (uint8_t *)music_pcm_start;

  while (1) {
    status = i2s_channel_write(tx_handle, data_ptr, music_pcm_end - data_ptr,
                               &bytes_written, portMAX_DELAY);
    if (status != ESP_OK) {
      ESP_LOGE(TAG, "I2S write failed with reason: %s",
               esp_err_to_name(status));
      abort();
    }
    if (bytes_written > 0) {
      ESP_LOGI(TAG, "I2S write successful, %d bytes written", bytes_written);
    } else {
      ESP_LOGE(TAG, "I2S write failed");
      abort();
    }
    data_ptr = (uint8_t *)music_pcm_start;
    // vTaskDelay(pdMS_TO_TICKS(1000));
  }
}