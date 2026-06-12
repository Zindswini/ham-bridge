#ifndef I2S_HANDLER_H
#define I2S_HANDLER_H

#include "driver/i2c_types.h"
#include "esp_err.h"

extern const uint8_t music_pcm_start[] asm("_binary_rick_pcm_start");
extern const uint8_t music_pcm_end[] asm("_binary_rick_pcm_end");

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t i2sDriverInit(void);
esp_err_t es8388CodecInit(i2c_master_bus_handle_t i2c_bus_handle);
void playI2sMusic(void);

#ifdef __cplusplus
{
#endif

#endif // I2S_HANDLER_H