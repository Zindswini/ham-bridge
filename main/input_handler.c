#include "input_handler.h"

#include "config.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "hal/gpio_types.h"
#include "screen_handler.h"
#include <math.h>
#include <string.h>

static const char *TAG = "INPUT_HANDLER";

static volatile uint64_t last_press_time = 0;

static volatile struct button_state current_button_state = {false, false,
                                                            false};
static volatile struct button_state previous_button_state = {false, false,
                                                             false};
static QueueHandle_t input_queue;

static void IRAM_ATTR buttonPollingISR(void *args) {
  // Debouncing
  previous_button_state = current_button_state;

  current_button_state.confirm_button_state =
      gpio_get_level(CONFIRM_BUTTON_PIN) != 0;
  current_button_state.increment_button_state =
      gpio_get_level(INCREMENT_BUTTON_PIN) != 0;
  current_button_state.decrement_button_state =
      gpio_get_level(DECREMENT_BUTTON_PIN) != 0;

  if (!previous_button_state.confirm_button_state &&
      current_button_state.confirm_button_state) {
    BaseType_t hp_task_woken = pdFALSE;
    button_types button_type = BUTTON_TYPE_CONFIRM;
    xQueueSendToBackFromISR(input_queue, &button_type, &hp_task_woken);
    if (hp_task_woken) {
      portYIELD_FROM_ISR();
    }
  }

  if (!previous_button_state.increment_button_state &&
      current_button_state.increment_button_state) {
    BaseType_t hp_task_woken = pdFALSE;
    button_types button_type = BUTTON_TYPE_INCREMENT;
    xQueueSendToBackFromISR(input_queue, &button_type, &hp_task_woken);
    if (hp_task_woken) {
      portYIELD_FROM_ISR();
    }
  }

  if (!previous_button_state.decrement_button_state &&
      current_button_state.decrement_button_state) {
    BaseType_t hp_task_woken = pdFALSE;
    button_types button_type = BUTTON_TYPE_DECREMENT;
    xQueueSendToBackFromISR(input_queue, &button_type, &hp_task_woken);
    if (hp_task_woken) {
      portYIELD_FROM_ISR();
    }
  }
}

void setupButtonGPIOTimer(void) {
  input_queue = xQueueCreate(10, sizeof(uint32_t));

  gpio_config_t gpio_conf = {
      .intr_type = GPIO_INTR_NEGEDGE,
      .mode = GPIO_MODE_INPUT,
      .pin_bit_mask = (1ULL << INCREMENT_BUTTON_PIN) |
                      (1ULL << DECREMENT_BUTTON_PIN) |
                      (1ULL << CONFIRM_BUTTON_PIN),
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .pull_up_en = GPIO_PULLUP_ENABLE,
  };
  ESP_LOGD(TAG, "Configuring GPIO");
  ESP_ERROR_CHECK(gpio_config(&gpio_conf));

  ESP_LOGD(TAG, "Setting up Button Polling ISR Timer");
  esp_timer_create_args_t timer_args = {.callback = buttonPollingISR,
                                        .name = "Button Polling Timer"};
  esp_timer_handle_t timer_handle = nullptr;
  ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle));
  ESP_ERROR_CHECK(
      esp_timer_start_periodic(timer_handle, 10 * 1000)); // Run every 10ms
  ESP_LOGD(TAG, "Set up Button Polling ISR Timer");
}

void processInputsTask(void) {
  uint32_t next_button = -1;
  while (true) {
    xQueueReceive(input_queue, &next_button, portMAX_DELAY);
    ESP_LOGD(TAG, "Received button press from queue: %i", next_button);
    processIncomingInput(next_button);
  }
}