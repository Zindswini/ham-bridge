#ifndef I2S_HANDLER_H
#define I2S_HANDLER_H

#include "driver/i2c_types.h"
#include "esp_err.h"
#include <cstddef>
#include <freertos/FreeRTOS.h>
#include <span>

// Create reference to PCM data provided by linker
// NOLINTBEGIN(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays,
// cppcoreguidelines-pro-bounds-array-to-pointer-decay)
extern const std::byte kMusicPcmStart[] asm("_binary_rick_pcm_start");
extern const std::byte kMusicPcmEnd[] asm("_binary_rick_pcm_end");
inline std::span<const std::byte> musicPcm() {
  return {kMusicPcmStart, static_cast<size_t>(kMusicPcmEnd - kMusicPcmStart)};
}
// NOLINTEND(cppcoreguidelines-avoid-c-arrays, modernize-avoid-c-arrays,
// cppcoreguidelines-pro-bounds-array-to-pointer-decay)

#ifdef __cplusplus
extern "C" {
#endif

// Struct for referencing buffers with actual size
struct Chunk {
  std::span<uint8_t> storage;
  size_t len = 0;
  [[nodiscard]] std::span<uint8_t> used() const { return storage.first(len); }
};

extern QueueHandle_t queue_filled;
extern QueueHandle_t queue_free;

esp_err_t initializeQueue();
esp_err_t i2sDriverInit(void);
esp_err_t es8388CodecInit(i2c_master_bus_handle_t i2c_bus_handle);
void i2SWriteTask(void *args);
void i2SReadTask(void *args);

#ifdef __cplusplus
}
#endif

#endif // I2S_HANDLER_H