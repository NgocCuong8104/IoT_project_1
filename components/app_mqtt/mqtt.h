#ifndef MQTT_H
#define MQTT_H

void mqtt_init(void);
void mqtt_send_status(void);
void mqtt_send_state(int relay_idx, int state);
void mqtt_send_response(int relay_idx, int state);
void mqtt_publish(const char *topic, const char *payload);
#endif