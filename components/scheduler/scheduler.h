#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define MAX_SLOTS 10 // Mỗi relay có thể có tối đa 10 khung giờ bật/tắt trong ngày
#define NUM_RELAYS 5 // Số lượng relay tối đa được hỗ trợ
#define CONFIG_VERSION 1
#define MAX_SUPPORTED_SENSORS 10
#define HYSTERESIS_TEMP 0.5f
#define HYSTERESIS_HUMI 2.0f
#define MAX_RULE_INPUTS 5

typedef struct {
    int h;
    int m;
} TimePoint_t;

typedef struct {
    TimePoint_t start;
    TimePoint_t stop;
    bool active; 
} TimeSlot_t;

typedef struct {
    uint8_t version;                   
    uint8_t day_mask;             
    TimeSlot_t slots[MAX_SLOTS]; 
} RelaySchedule_t;

typedef struct {
    uint8_t version;                   
    bool active;
    char type[10];

    uint8_t day_mask;     
    TimePoint_t start;  
    TimePoint_t stop;

    float temp_high;
    float temp_low;
    float hysteresis_temp;             
    float humi_high;
    float humi_low;
    float hysteresis_humi;           

    float current_temp;
    float current_humi;

    int action1_relays[NUM_RELAYS];   
    int action2_relays[NUM_RELAYS];   

} sensor_config_t;

typedef struct {
    int threshold_value;        // Giá trị (0 hoặc 1)
    int action_relays[NUM_RELAYS];  // Hành động relay tương ứng
} InputAction_t;

typedef struct {
    uint8_t version;
    uint8_t day_mask;           // Ngày hoạt động
    TimeSlot_t slots[MAX_SLOTS]; // Thời gian hoạt động  
    int constraint[NUM_RELAYS];  // Điều kiện relay (-1: bất kỳ, 0: tắt, 1: bật)
    InputAction_t actions[MAX_RULE_INPUTS]; // Tối đa 5 thresholds/actions
    int num_actions;            // Số lượng actions
    bool active;                
} InputConfig_t;

typedef struct {
    uint8_t version; // Phiên bản cấu hình
    InputConfig_t inputs[3];    // Input 1, 2, 3
} input_state_t;

// lưu trữ thông tin cảm biến : phiên bản, số lượng cảm biến, địa chỉ 
typedef struct {
    uint8_t version;                           // Configuration version
    uint8_t count;                             // Total number of connected sensors
    uint8_t addresses[MAX_SUPPORTED_SENSORS];  // Array of sensor slave IDs
} sensor_network_t;

void scheduler_init(void);
void scheduler_update_from_json(const char *json_str);
void scheduler_set_on_time(int relay_idx, const char *time_str);
void scheduler_set_off_time(int relay_idx, const char *time_str);
void scheduler_check_at_boot(void); // Kiểm tra các lịch trình đã bị bỏ lỡ trong thời gian mất kết nối và khôi phục trạng thái relay tương ứng
void scheduler_process_sensor(float current_value);
void scheduler_process_input(int input_idx, int new_state);

SemaphoreHandle_t scheduler_get_mutex(void);
void scheduler_lock(void);
void scheduler_unlock(void);

extern sensor_config_t sensor_conf;
extern RelaySchedule_t schedules[NUM_RELAYS]; // Lịch trình cho NUM_RELAYS relay
extern input_state_t input_state;
extern sensor_network_t sensor_net;

void add_new_sensor_to_network(uint8_t new_id);
#endif