#ifndef INPUT_H
#define INPUT_H

#include "esp_err.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CONTACT_1_PIN GPIO_NUM_32
#define CONTACT_2_PIN GPIO_NUM_34
#define CONTACT_3_PIN GPIO_NUM_35

// thời gian chống rung
#define DEBOUNCE_MS 50  

// Callback function typedef - được gọi khi trạng thái thay đổi
typedef void (*dry_contact_callback_t)(int index, int state);

// Khởi tạo 3 tiếp điểm khô
void dry_contact_input_init(void);

// Đọc trạng thái tiếp điểm: index 1-3, return 0 (MỞ) hoặc 1 (ĐÓNG)
int dry_contact_read_state(int index);

// Đăng ký callback khi trạng thái thay đổi
void dry_contact_register_callback(dry_contact_callback_t cb);

// Bắt đầu task monitor
void dry_contact_start_monitor(void);

// Dừng task monitor
void dry_contact_stop_task(void);

// In ra trạng thái hiện tại
void dry_contact_print_status(void);

#ifdef __cplusplus
extern "C" { }
#endif

#endif // INPUT_H