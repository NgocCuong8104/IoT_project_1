#include "led_status.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "LED_STATUS";

static TaskHandle_t led_task_handle = NULL;
static volatile bool is_continuous_blinking = false;
static volatile int continuous_on_ms = 500;
static volatile int continuous_off_ms = 500;

static void led_blink_task(void *arg) {
    while (1) {
        if (is_continuous_blinking) {
            gpio_set_level(LED_STATUS_PIN, 0); // Bật LED
            vTaskDelay(pdMS_TO_TICKS(continuous_on_ms));
            // Phải kiểm tra lại cờ, vì trong lúc delay có thể đã có mạng và gọi ngắt
            if (!is_continuous_blinking) continue; 
            gpio_set_level(LED_STATUS_PIN, 1); // Tắt LED
            vTaskDelay(pdMS_TO_TICKS(continuous_off_ms));
        } else {
            // Khi không cần nháy, task ngủ 100ms để nhường hoàn toàn CPU cho hệ thống
            vTaskDelay(pdMS_TO_TICKS(100)); 
        }
    }
}

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
    xTaskCreate(led_blink_task, "led_blink_task", 2048, NULL, 5, &led_task_handle);
    ESP_LOGI(TAG, "LED Status initialized on GPIO%d", LED_STATUS_PIN);
}

void led_status_on(void)
{
    is_continuous_blinking = false;
    gpio_set_level(LED_STATUS_PIN, 0);
}

void led_status_off(void)
{
    is_continuous_blinking = false;
    gpio_set_level(LED_STATUS_PIN, 1);
}

void led_status_blink(int count, int on_ms, int off_ms)
{
    is_continuous_blinking = false;
    for (int i = 0; i < count; i++) {
        led_status_on();
        vTaskDelay(pdMS_TO_TICKS(on_ms));
        led_status_off();
        vTaskDelay(pdMS_TO_TICKS(off_ms));
    }
}

void led_status_start_continuous_blink(int on_ms, int off_ms)
{
    is_continuous_blinking = true;
    continuous_on_ms = on_ms;
    continuous_off_ms = off_ms;
}

void led_status_stop_continuous_blink(void)
{
    is_continuous_blinking = false;
    led_status_off();
}
