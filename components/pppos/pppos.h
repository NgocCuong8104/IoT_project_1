#pragma once
#include <stdbool.h>
#include <stdint.h>

void pppos_init(void);
void pppos_start_connect(void);
bool pppos_is_connected(void);
void pppos_disconnect(void);
void pppos_mqtt_task(void *arg); 