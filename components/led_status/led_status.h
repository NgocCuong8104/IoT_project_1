#ifndef LED_STATUS_H
#define LED_STATUS_H

#include <stdbool.h>

#define LED_STATUS_PIN  33
void led_status_init(void);
void led_status_on(void);
void led_status_off(void);
void led_status_blink(int count, int on_ms, int off_ms);

void led_status_start_continuous_blink(int on_ms, int off_ms);
void led_status_stop_continuous_blink(void);
#endif
