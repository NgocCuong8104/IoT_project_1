#ifndef MQTT_H
#define MQTT_H

#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

bool mqtt_is_connected(void);
extern void sensor_data_lock(void);
extern void sensor_data_unlock(void);
void mqtt_init(void);
void mqtt_send_status(void);
void mqtt_send_state(int relay_idx, int state);
void mqtt_send_response(int relay_idx, int state);
void mqtt_publish(const char *topic, const char *payload);
#endif