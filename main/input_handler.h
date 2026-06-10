#ifndef INPUT_HANDLER_H
#define INPUT_HANDLER_H

#include "config.h"
#include "driver/gpio.h"
#include "driver/gpio_filter.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum {
  BUTTON_TYPE_INCREMENT = 0,
  BUTTON_TYPE_DECREMENT = 1,
  BUTTON_TYPE_CONFIRM = 2
} button_types;

void setup_gpio(void);
void process_inputs(void *args);

#endif