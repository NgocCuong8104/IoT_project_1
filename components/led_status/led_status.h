#ifndef LED_STATUS_H
#define LED_STATUS_H

#include <stdbool.h>

#define LED_STATUS_PIN  33
void led_status_init(void);
void led_status_on(void);
void led_status_off(void);
void led_status_blink(int count, int on_ms, int off_ms);

#endif
