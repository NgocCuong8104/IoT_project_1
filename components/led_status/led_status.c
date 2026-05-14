#include "led_status.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "LED_STATUS";

void led_status_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_STATUS_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    led_status_off();
    ESP_LOGI(TAG, "LED Status initialized on GPIO%d", LED_STATUS_PIN);
}

void led_status_on(void)
{
    gpio_set_level(LED_STATUS_PIN, 0);
}

void led_status_off(void)
{
    gpio_set_level(LED_STATUS_PIN, 1);
}

void led_status_blink(int count, int on_ms, int off_ms)
{
    for (int i = 0; i < count; i++) {
        led_status_on();
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        led_status_off();
        vTaskDelay(pdMS_TO_TICKS(off_ms));
    }
}
