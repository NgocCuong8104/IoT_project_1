#include "relay.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include <stdio.h>
#include <scheduler.h>

static const char *TAG = "RELAY";

static int relay_pins[NUM_RELAYS] = {RL1_PIN, RL2_PIN, RL3_PIN, RL4_PIN, RL5_PIN};
static int relay_states[NUM_RELAYS] = {0};

void save_relay_nvs(int index, int state) {
    nvs_handle_t nvs_handle;
    esp_err_t err;
    char key[32];

    snprintf(key, sizeof(key), "relay_%d", index + 1);

    err = nvs_open("relay_storage", NVS_READWRITE, &nvs_handle);
    if (err == ESP_OK) {
        nvs_set_i32(nvs_handle, key, state);
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
    } 
}

int load_relay_nvs(int index) {
    nvs_handle_t nvs_handle;
    esp_err_t err;
    int32_t state = 0;
    char key[32];

    snprintf(key, sizeof(key), "relay_%d", index + 1);

    err = nvs_open("relay_storage", NVS_READONLY, &nvs_handle);
    if (err == ESP_OK) {
        err = nvs_get_i32(nvs_handle, key, &state);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            state = 0; 
        }
        nvs_close(nvs_handle);
    }
    return state;
}

void relay_init(void) {
    gpio_config_t io_conf = {
        .intr_type = GPIO_INTR_DISABLE,
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 0, 
        .pull_down_en = 0,
        .pull_up_en = 0,
    };

    for(int i=0; i<NUM_RELAYS; i++) {
        io_conf.pin_bit_mask |= (1ULL << relay_pins[i]);
    }
    gpio_config(&io_conf);
    
    for (int i=0; i<NUM_RELAYS; i++) {
        int save_state = load_relay_nvs(i);
        relay_states[i] = save_state;   
        gpio_set_level(relay_pins[i], save_state);
        ESP_LOGI(TAG, "Initialized RL%d to %d", i + 1, save_state);
    }
    ESP_LOGI(TAG, "Initialized %d Relays", NUM_RELAYS);
}

int relay_get_status(int relay_index) {
    if (relay_index < 1 || relay_index > NUM_RELAYS) return 0; 
    return relay_states[relay_index - 1];
}

void relay_set(int index, int state, bool save_to_flash) {
    if (index < 1 || index > NUM_RELAYS) return;
    int i = index - 1; 

    if (relay_states[i] != state) {
        relay_states[i] = state;
        gpio_set_level(relay_pins[i], state);
        if (save_to_flash) save_relay_nvs(i, state);
        ESP_LOGI(TAG, "RL%d -> %d", index, state);
    }
}

void relay_commit_nvs(int index, int state) {
    save_relay_nvs(index - 1, state);
}

void relay_on(int index) {
    relay_set(index, 1, false);
}

void relay_off(int index) {
    relay_set(index, 0, false);
}

void relay_toggle(int index) {
    if (index < 1 || index > NUM_RELAYS) return;
    int i = index - 1;
    int new_state = !relay_states[i];
    relay_set(index, new_state, true);  
    // save_relay_nvs(i, new_state);  
}

void relay_all_off(void) {
    for(int i=0; i<NUM_RELAYS; i++) {
        relay_set(i + 1, 0, false);
    }
}

void relay_toggle_offline(int index) {
    if (index < 1 || index > NUM_RELAYS) return;
    int i = index - 1;
    
    relay_states[i] = !relay_states[i];
    gpio_set_level(relay_pins[i], relay_states[i]);
    relay_set(index, relay_states[i], false);

    ESP_LOGI(TAG, "Relay %d Toggle (Offline Effect)", index);
}