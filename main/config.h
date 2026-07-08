#ifndef CONFIG_H
#define CONFIG_H

#include <driver/i2s_types.h>
#include <soc/gpio_num.h>

// OLED Display and ES8388 Control
#define PIN_SDA GPIO_NUM_1
#define PIN_SCL GPIO_NUM_15

// ES8388 I2S Pins
#define PIN_DOUT GPIO_NUM_3
#define PIN_LRCLK GPIO_NUM_17
#define PIN_DIN GPIO_NUM_16
#define PIN_SCLK GPIO_NUM_2
#define PIN_MCLK GPIO_NUM_0
#define PIN_PA GPIO_NUM_6
#define VCC_VOLTAGE 3.3F

// I2S Global Sample Rate
#define I2S_SAMPLE_RATE 24000
#define I2S_BIT_WIDTH I2S_DATA_BIT_WIDTH_16BIT
#define I2S_MCLK_MULITPLE I2S_MCLK_MULTIPLE_512

// Front Interface Button Pins
#define INCREMENT_BUTTON_PIN GPIO_NUM_38
#define DECREMENT_BUTTON_PIN GPIO_NUM_40
#define CONFIRM_BUTTON_PIN GPIO_NUM_39
#define DEBOUNCE_DELAY_MS 50

// HTTPS Server Settings
#define MAX_HTTP_CLIENTS 4

// TLS Certificate Settings
#define ENABLE_SSL false
#define KEY_BITS 2048
#define NVS_CERT_NAMESPACE "tls_store"
#define NVS_TLS_CERT_KEY "cert_val"
#define NVS_TLS_KEY_KEY "key_val"

// Audio Buffer Settings
#define BUFFER_SLOTS 6
#define BUFFER_SLOT_BYTES (1024 * 128)

#endif