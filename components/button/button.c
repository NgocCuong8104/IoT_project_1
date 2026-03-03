// #include <stdint.h>
// #include <stdbool.h>
#include "button.h"
#include "relay.h" 
#include "wifi.h"
#include "pppos.h"
#include "wifi.h"  
#include "pppos.h"
#include "driver/gpio.h"
#include "nvs_flash.h"
#include "wifi_provisioning/manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "mqtt.h"
// #include "freertos/queue.h"

static const char *TAG = "BUTTON";
  
#define WARNING_MS    4000     
#define DEBOUNCE_MS   50      
// #define CLICK_COOLDOWN_MS  300
  
extern int app_mode;

extern void wifi_start_smartconfig(void);
extern void pppos_start_connect(void);
void save_mode_and_restart(int32_t mode);

typedef void (*long_press_cb_t)(void);

typedef struct {
    int pin;            
    int relay_idx;    
    long_press_cb_t lp_cb; 
    bool is_pressed;
    bool long_press_handled;
    bool warning_printed;
    int64_t press_start_time;
    int64_t last_click_time;
} Button_t;

static Button_t buttons[5] = {
    {BT1_PIN, 1, wifi_start_smartconfig, false, false, false, 0, 0}, 
    {BT2_PIN, 2, pppos_start_connect,     false, false, false, 0, 0}, 
    {BT3_PIN, 3, NULL,                   false, false, false, 0, 0}, 
    {BT4_PIN, 4, NULL,                   false, false, false, 0, 0},
    {BT5_PIN, 5, NULL,                   false, false, false, 0, 0}
};

static void clear_wifi_credentials(void) {
    wifi_prov_mgr_reset_provisioning();
    
    nvs_handle_t nvs_handle;
    if (nvs_open("nvs.net80211", NVS_READWRITE, &nvs_handle) == ESP_OK) {
        nvs_erase_all(nvs_handle);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    }
    ESP_LOGW(TAG, "WiFi credentials cleared!");
}

void trigger_long_press_action(int btn_index) {
    int relay_id = buttons[btn_index].relay_idx;
    
    if (btn_index == 0) ESP_LOGW(TAG, "(WIFI)!");
    else if (btn_index == 1) ESP_LOGW(TAG, "(4G)!");

    for (int k = 0; k < 3; k++) {
        relay_on(relay_id);
        vTaskDelay(pdMS_TO_TICKS(100));
        relay_off(relay_id);
        vTaskDelay(pdMS_TO_TICKS(100)); 
    }

    relay_off(relay_id); 

    if (btn_index == 0) {
        clear_wifi_credentials(); 
        save_mode_and_restart(1);
    } 
    else if (btn_index == 1) {
        save_mode_and_restart(2);
    }

    if (buttons[btn_index].lp_cb != NULL) {
        buttons[btn_index].lp_cb();
    }
}

void button_task(void *arg) {
    while (1) {
        int64_t now = esp_timer_get_time() / 1000; 

        for (int i = 0; i < 5; i++) {
            int level = gpio_get_level(buttons[i].pin);

            if (level == 1) {
                if (!buttons[i].is_pressed) {
                    buttons[i].is_pressed = true;
                    buttons[i].press_start_time = now;
                    buttons[i].long_press_handled = false;
                    buttons[i].warning_printed = false;
                } 
                else {
                    bool ignore = false;

                    if(i == 0 && app_mode == 1) {
                        ignore = true;
                    } 
                    if (i == 1 && app_mode == 2) {
                        ignore = true;
                    }
                    if (!ignore) {
                        int64_t duration = now - buttons[i].press_start_time;

                        if (buttons[i].lp_cb != NULL && !buttons[i].warning_printed && duration > WARNING_MS && duration < LONG_PRESS_MS) {
                            if (i == 0) ESP_LOGI(TAG, "Giu them 2s cai Wifi...");
                            if (i == 1) ESP_LOGI(TAG, "Giu them 2s bat 4G...");
                            buttons[i].warning_printed = true;
                        }

                        if (buttons[i].lp_cb != NULL && !buttons[i].long_press_handled && duration > LONG_PRESS_MS) {
                            trigger_long_press_action(i);
                            buttons[i].long_press_handled = true;
                        }
                    }
                }
            }
            else {
                if (buttons[i].is_pressed) {
                    buttons[i].is_pressed = false; 
                    int64_t duration = now - buttons[i].press_start_time;

                    if (!buttons[i].long_press_handled && duration > DEBOUNCE_MS) {
                        // if (now - buttons[i].last_click_time > CLICK_COOLDOWN_MS) {
                            buttons[i].last_click_time = now;
                            relay_toggle(buttons[i].relay_idx);
                            int new_state = relay_get_status(buttons[i].relay_idx);
                            // if (relay_queue != NULL) {
                            //     relay_event_t evt;
                            //     evt.relay_idx = buttons[i].relay_idx;
                            //     evt.new_state = new_state;
                            //     xQueueSend(relay_queue, &evt, 0);
                            // }
                            // relay_commit_nvs(buttons[i].relay_idx, new_state);

                            mqtt_send_state(buttons[i].relay_idx, new_state);
                        // }
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
void button_init(void) {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_INPUT,
        .pin_bit_mask = 0,
        .pull_down_en = 1,
        .pull_up_en = 0, 
    };
    for(int i=0; i<5; i++) {
        io_conf.pin_bit_mask |= (1ULL << buttons[i].pin);
    }
    gpio_config(&io_conf);

    xTaskCreate(button_task, "btn_task", 8192, NULL, 5, NULL);
    ESP_LOGI(TAG, "Button Service Started (5 Buttons)");
}