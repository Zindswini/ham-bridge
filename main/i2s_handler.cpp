#include "i2s_handler.h"
#include "config.h"

#include "driver/i2c_types.h"
#include "driver/i2s_common.h"
#include "driver/i2s_types.h"
#include "esp_err.h"
#include "esp_log.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <freertos/FreeRTOS.h>

// Espressif I2S Codec Library
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_codec_dev_types.h"
#include "freertos/idf_additions.h"
#include "freertos/projdefs.h"
#include "hal/i2s_types.h"
#include "portmacro.h"
#include "soc/clk_tree_defs.h"

// I2S Handles
static i2s_chan_handle_t tx_handle = nullptr;
static i2s_chan_handle_t rx_handle = nullptr;

static const char *tag = "I2S_HANDLER";

// Allocate static buffers
EXT_RAM_BSS_ATTR static std::array<std::array<uint8_t, BUFFER_SLOT_BYTES>,
                                   BUFFER_SLOTS>
    pool;
static std::array<Chunk, BUFFER_SLOTS> chunks;

// Allocate static queues
static StaticQueue_t queue_free_chunks_ctrl;
static StaticQueue_t queue_filled_chunks_ctrl;
static std::array<uint8_t, BUFFER_SLOTS * sizeof(Chunk *)> queue_free_store;
static std::array<uint8_t, BUFFER_SLOTS * sizeof(Chunk *)> queue_filled_store;
QueueHandle_t queue_free = nullptr;
QueueHandle_t queue_filled = nullptr;

esp_err_t i2sDriverInit(void) {
  i2s_chan_config_t chan_cfg =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true;
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle));

  i2s_std_config_t std_cfg = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(I2S_SAMPLE_RATE),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_BIT_WIDTH,
                                                      I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = PIN_MCLK,
              .bclk = PIN_SCLK,
              .ws = PIN_LRCLK,
              .dout = PIN_DOUT,
              .din = PIN_DIN,
              .invert_flags =
                  {
                      .mclk_inv = 0,
                      .bclk_inv = 0,
                      .ws_inv = 0,
                  },
          },
  };
  std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULITPLE;

  ESP_LOGD(tag, "Initializing I2S Channels");
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
  ESP_LOGD(tag, "Initialized I2S Channels");

  ESP_LOGD(tag, "Enabling I2S Channels");
  ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
  ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
  ESP_LOGD(tag, "Enabled I2S Channels");
  return ESP_OK;
}

esp_err_t es8388CodecInit(i2c_master_bus_handle_t i2c_bus_handle) {
  // Create I2S control interface with I2C bus handle
  ESP_LOGD(tag, "Initializing I2S I2C Config");
  audio_codec_i2c_cfg_t i2s_i2c_cfg = {
      .port = I2C_NUM_0,
      .addr = ES8388_CODEC_DEFAULT_ADDR,
      .bus_handle = i2c_bus_handle,
  };
  const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2s_i2c_cfg);
  if (ctrl_if == nullptr) {
    ESP_LOGE(tag, "Failed to get i2s i2c control object!");
    return ESP_FAIL;
  }

  ESP_LOGD(tag, "Initialized I2S I2C Config");

  ESP_LOGD(tag, "Initializing I2S Config");
  audio_codec_i2s_cfg_t i2s_cfg = {
      .port = I2S_NUM_0,
      .rx_handle = rx_handle,
      .tx_handle = tx_handle,
      .clk_src = I2S_CLK_SRC_DEFAULT,
  };
  const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
  if (data_if == nullptr) {
    ESP_LOGE(tag, "Failed to get i2s data interface!");
    return ESP_FAIL;
  }
  ESP_LOGD(tag, "Initialized I2S Config");

  ESP_LOGD(tag, "Initializing I2S GPIO Config");
  const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
  if (gpio_if == nullptr) {
    ESP_LOGE(tag, "Failed to get i2s gpio interface!");
    return ESP_FAIL;
  }
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
  if (es8388_if == nullptr) {
    ESP_LOGE(tag, "Failed to get i2s es8388 interface!");
    return ESP_FAIL;
  }
  ESP_LOGD(tag, "Initialized I2S GPIO Config");

  ESP_LOGD(tag, "Initializing I2S Device Config");
  esp_codec_dev_cfg_t dev_cfg = {
      .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
      .codec_if = es8388_if,
      .data_if = data_if,
  };
  esp_codec_dev_handle_t codec_handle = esp_codec_dev_new(&dev_cfg);
  if (codec_handle == nullptr) {
    ESP_LOGE(tag, "Failed to get i2s codec handle!");
    return ESP_FAIL;
  }
  ESP_LOGD(tag, "Initialized I2S Device Config");

  ESP_LOGD(tag, "Initializing I2S Sample Config");
  esp_codec_dev_sample_info_t sample_cfg = {
      .bits_per_sample = I2S_BIT_WIDTH,
      .channel = 2,
      .channel_mask = 0x03,
      .sample_rate = I2S_SAMPLE_RATE,
      .mclk_multiple = I2S_MCLK_MULITPLE,
  };

  if (esp_codec_dev_open(codec_handle, &sample_cfg) != ESP_CODEC_DEV_OK) {
    ESP_LOGE(tag, "Open codec device failed");
    return ESP_FAIL;
  }

  if (esp_codec_dev_set_out_vol(codec_handle, 100) != ESP_CODEC_DEV_OK) {
    ESP_LOGE(tag, "set output digital gain failed");
    return ESP_FAIL;
  }

  // Set LOUT DAC Gain to 0db (Previously set to -30db in open func)
  esp_err_t ret = ESP_CODEC_DEV_OK;
  int val_to_write = 0x1E; // 0db
  // int val_to_write = 0x0F; // -15db

  // Set Register ES8388_DACCONTROL26 (0x30) (LOUT2 Gain) = 0dB
  ret |= ctrl_if->write_reg(ctrl_if, 0x30, 1, &val_to_write, 1);

  // Set Register ES8388_DACCONTROL27 (0x31) (LOUT2 Gain) = 0dB
  ret |= ctrl_if->write_reg(ctrl_if, 0x31, 1, &val_to_write, 1);

  if (ret != ESP_CODEC_DEV_OK) {
    ESP_LOGE(tag, "set output physical gain failed: Code %d", ret);
    return ESP_FAIL;
  }

  // Set input to LINE2 (IQ)
  // val_to_write = 0x50;
  // ret |= ctrl_if->write_reg(ctrl_if, 0x0a, 1, &val_to_write, 1);

  if (ret != ESP_CODEC_DEV_OK) {
    ESP_LOGE(tag, "set input select Line2 failed: Code %d", ret);
    return ESP_FAIL;
  }

  if (esp_codec_dev_set_in_gain(codec_handle, 8) != ESP_CODEC_DEV_OK) {
    ESP_LOGE(tag, "set input volume failed");
    return ESP_FAIL;
  }

  ESP_LOGD(tag, "Initialized I2S Sample Config");
  return ESP_OK;
}

esp_err_t initializeQueue() {
  // Setup the queues using the static memory
  queue_free =
      xQueueCreateStatic(BUFFER_SLOTS, sizeof(Chunk *), queue_free_store.data(),
                         &queue_free_chunks_ctrl);
  queue_filled =
      xQueueCreateStatic(BUFFER_SLOTS, sizeof(Chunk *),
                         queue_filled_store.data(), &queue_filled_chunks_ctrl);

  // Initialize free queue with valid Chunk objects
  for (size_t i = 0; i < BUFFER_SLOTS; i++) {
    Chunk *chunk = &(chunks.at(i));
    chunk->storage = std::span<uint8_t>{pool.at(i)};
    xQueueSend(queue_free, static_cast<void *>(&chunk), 0);
  }
  return ESP_OK;
}

void i2SWriteTask(void *args __unused) {
  esp_err_t status = ESP_OK;
  size_t bytes_written = 0;

  while (true) {
    status = i2s_channel_write(tx_handle, musicPcm().data(), musicPcm().size(),
                               &bytes_written, portMAX_DELAY);
    if (status != ESP_OK) {
      ESP_LOGE(tag, "I2S write failed with reason: %s",
               esp_err_to_name(status));
      abort();
    }
    if (bytes_written > 0) {
      ESP_LOGI(tag, "I2S write successful, %d bytes written", bytes_written);
    } else {
      ESP_LOGE(tag, "I2S write failed");
      abort();
    }
    // vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void i2SReadTask(void *args __unused) {
  // TODO(Zindswini): Encode w/ ADPCM before submitting to queue

  while (true) {
    // Get chunk from queue of free chunks
    Chunk *chunk = nullptr;
    ESP_LOGD(tag, "Attempting to recieve from free queue");
    if (xQueueReceive(queue_free, static_cast<void *>(&chunk), 0) != pdTRUE) {
      ESP_LOGW(tag, "No chunk in free queue! HTTP web side must be too slow!");
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    ESP_LOGD(tag, "Got free chunk with base addr: %p", chunk->storage.data());

    // Read from I2S DMA buffer into chunk we got
    esp_err_t status = ESP_OK;
    size_t bytes_read = 0;
    status =
        i2s_channel_read(rx_handle, chunk->storage.data(),
                         chunk->storage.size(), &bytes_read, portMAX_DELAY);

    if (status != ESP_OK) {
      ESP_LOGE(tag, "I2S Read failed with reason: %s", esp_err_to_name(status));
      abort();
    }
    if (bytes_read > 0) {
      ESP_LOGI(tag, "I2S Read Successful, %d bytes read to addr %p", bytes_read,
               chunk->storage.data());
      chunk->len = bytes_read;
      if (xQueueSend(queue_filled, static_cast<void *>(&chunk), 0) != pdTRUE) {
        ESP_LOGE(tag, "Failed to put chunk into filled queue!");
      } else {
        ESP_LOGD(tag, "Put chunk in filled queue");
      }
    } else {
      ESP_LOGE(tag, "I2S Read Failed");

      // Data is invalid, put back into free queue to be overwritten
      xQueueSend(queue_free, static_cast<void *>(&chunk), portMAX_DELAY);

      // Backoff to prevent tight loop
      vTaskDelay(pdMS_TO_TICKS(400));
    }
  }
}