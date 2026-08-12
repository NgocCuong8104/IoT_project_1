#ifndef RELAY_H
#define RELAY_H

#define RL1_PIN  13
#define RL2_PIN  12
#define RL3_PIN  14
#define RL4_PIN  27
#define RL5_PIN  26

#include <stdint.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// typedef struct {
//     int relay_idx;
//     int new_state;
// } relay_event_t;

// extern QueueHandle_t relay_queue;

void relay_init(void);
void relay_set(int index, int state, bool save_to_flash);
void relay_on(int index);   
void relay_off(int index); 
void relay_toggle(int index); 
void relay_all_off(void); 
// int relay_get_state(int index);
void relay_toggle_offline(int index);
void save_relay_nvs(int index, int state);
int load_relay_nvs(int index);
int relay_get_status(int relay_index);
void relay_commit_nvs(int index, int state);
#endif