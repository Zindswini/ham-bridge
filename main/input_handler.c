#include "input_handler.h"

static const char* TAG = "INPUT_HANDLER";

static volatile uint64_t last_press_time = 0;
static QueueHandle_t input_queue;

static void IRAM_ATTR button_isr(void *args)
{
    // Debouncing
    uint64_t now = esp_timer_get_time();
    if((now - last_press_time) > (DEBOUNCE_DELAY_MS * 1000))
    {
        // Get which button was pressed from the args
        button_types button_type = (uint32_t)args;
        BaseType_t hp_task_woken = pdFALSE;

        xQueueSendToBackFromISR(input_queue, &button_type, &hp_task_woken);
        if(hp_task_woken){
            portYIELD_FROM_ISR();
        }
        last_press_time = now;
    }
}

void setup_gpio(void)
{
    input_queue = xQueueCreate(10, sizeof(uint32_t));

    gpio_config_t gpio_conf = {
        .intr_type = GPIO_INTR_NEGEDGE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = (1ULL << INCREMENT_BUTTON_PIN) | (1ULL << DECREMENT_BUTTON_PIN) | (1ULL << CONFIRM_BUTTON_PIN),
        .pull_down_en = 0,
        .pull_up_en = 1,
    };
    ESP_LOGD(TAG, "Configuring GPIO");
    ESP_ERROR_CHECK(gpio_config(&gpio_conf));

    gpio_pin_glitch_filter_config_t filter_conf = {
        .clk_src = GLITCH_FILTER_CLK_SRC_DEFAULT,
        .gpio_num = INCREMENT_BUTTON_PIN,
    };
    gpio_glitch_filter_handle_t increment_filter_handle;
    ESP_LOGD(TAG, "Configuring Glitch Filter");
    ESP_ERROR_CHECK(gpio_new_pin_glitch_filter(&filter_conf, &increment_filter_handle));
    ESP_ERROR_CHECK(gpio_glitch_filter_enable(increment_filter_handle));

    ESP_LOGD(TAG, "Installing ISR Service");
    esp_err_t err = gpio_install_isr_service(0);
    ESP_LOGD(TAG, "ISR Service result: %i", err);

    ESP_LOGD(TAG, "Adding ISR Handlers");
    ESP_ERROR_CHECK(gpio_isr_handler_add(INCREMENT_BUTTON_PIN, &button_isr, (void*)(uint32_t)(BUTTON_TYPE_INCREMENT)));
    ESP_ERROR_CHECK(gpio_isr_handler_add(DECREMENT_BUTTON_PIN, &button_isr, (void*)(uint32_t)(BUTTON_TYPE_DECREMENT)));
    ESP_ERROR_CHECK(gpio_isr_handler_add(CONFIRM_BUTTON_PIN,   &button_isr, (void*)(uint32_t)(BUTTON_TYPE_CONFIRM)));

}

void process_inputs(void *args)
{
    uint32_t next_button;
    while(true)
    {
        xQueueReceive(input_queue, &next_button, portMAX_DELAY);
        ESP_LOGI(TAG, "Received button press from queue: %i", next_button);
    }
    return;
}