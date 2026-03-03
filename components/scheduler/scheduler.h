#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <stdbool.h>
#include <stdint.h>

#define MAX_SLOTS 10

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
    uint8_t day_mask;             
    TimeSlot_t slots[MAX_SLOTS]; 
} RelaySchedule_t;

typedef struct {
    bool active;
    char type[10];

    uint8_t day_mask;     
    TimePoint_t start;  
    TimePoint_t stop;

    float temp_high;
    float temp_low;

    float humi_high;
    float humi_low;

    float current_temp;
    float current_humi;

    int action1_RL1; int action1_RL2; int action1_RL3; int action1_RL4; int action1_RL5;
    int action2_RL1; int action2_RL2; int action2_RL3; int action2_RL4; int action2_RL5;

} sensor_config_t;

void scheduler_init(void);
void scheduler_update_from_json(const char *json_str);
void scheduler_set_on_time(int relay_idx, const char *time_str);
void scheduler_set_off_time(int relay_idx, const char *time_str);
void scheduler_check_at_boot(void);
void scheduler_process_sensor(float current_value);

//extern ManualOverride_t manual_overrides[5];
extern sensor_config_t sensor_conf;
extern RelaySchedule_t schedules[5];
#endif