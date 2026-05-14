#include <stdio.h>
#include "input.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"

static const char *TAG = "DRY_CONTACT";

typedef struct {
    int current_state;           // Trạng thái hiện tại (0: MỞ, 1: ĐÓNG)
    int previous_state;          // Trạng thái trước
    int debounce_count;          // bộ đếm chống rung 
    int64_t last_change_time;    // Thời gian thay đổi cuối cùng
} contact_state_t;

static contact_state_t contact_states[3] = { // khởi tạo tất cả trạng thái và bộ đếm về 0
    {0, 0, 0, 0},
    {0, 0, 0, 0},
    {0, 0, 0, 0}
};

// Callback function pointer
static dry_contact_callback_t user_callback = NULL;

// Task handle
static TaskHandle_t contact_task_handle = NULL;

void dry_contact_input_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << CONTACT_1_PIN) | (1ULL << CONTACT_2_PIN) | (1ULL << CONTACT_3_PIN),
        .mode = GPIO_MODE_INPUT,
        //.pull_up_en = GPIO_PULLUP_ENABLE,     // Bật điện trở kéo lên
        .pull_up_en = GPIO_PULLUP_DISABLE,     // Tắt điện trở kéo lên vì sử dụng pull-up bên ngoài
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE        // Không dùng ngắt, chỉ đọc trạng thái
    };
    
    esp_err_t ret = gpio_config(&io_conf);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure GPIO!");
        return;
    }
    
    // Đọc trạng thái ban đầu
    contact_states[0].current_state = (gpio_get_level(CONTACT_1_PIN) == 0) ? 1 : 0; // đảo logic vì pull-up bên ngoài, 0 = ĐÓNG, 1 = MỞ
    contact_states[1].current_state = (gpio_get_level(CONTACT_2_PIN) == 0) ? 1 : 0;
    contact_states[2].current_state = (gpio_get_level(CONTACT_3_PIN) == 0) ? 1 : 0;
    
    contact_states[0].previous_state = contact_states[0].current_state; // khởi tạo trạng thái trước bằng trạng thái hiện tại để tránh báo thay đổi sai khi bắt đầu giám sát
    contact_states[1].previous_state = contact_states[1].current_state;
    contact_states[2].previous_state = contact_states[2].current_state;
    
    ESP_LOGI(TAG, "Dry Contacts initialized on GPIO 32, 34, 35");
    ESP_LOGI(TAG, "Initial states - Contact1: %d, Contact2: %d, Contact3: %d",
             contact_states[0].current_state, 
             contact_states[1].current_state, 
             contact_states[2].current_state);
}

// State Reading 
int dry_contact_read_state(int index) {
    if (index < 1 || index > 3) {
        ESP_LOGW(TAG, "Invalid contact index: %d (must be 1-3)", index);
        return -1;
    }
    return contact_states[index - 1].current_state;
}

// đăng ký callback để thông báo khi trạng thái thay đổi
void dry_contact_register_callback(dry_contact_callback_t cb) {
    user_callback = cb;
    if (cb != NULL) {
        ESP_LOGI(TAG, "Callback registered");
    }
}

// giám sát trạng thái tiếp điểm khô và gọi callback khi có sự thay đổi
static void dry_contact_monitor_task(void *arg) {
    const int DEBOUNCE_THRESHOLD = DEBOUNCE_MS / 10;  // Chia 10ms polling interval (hỏi trạng thái mỗi 10ms)
    const int READ_INTERVAL_MS = 10;
    
    ESP_LOGI(TAG, "Dry Contact Monitor Task started");
    
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(READ_INTERVAL_MS));
        
        // Mảng GPIO pins
        gpio_num_t pins[3] = {CONTACT_1_PIN, CONTACT_2_PIN, CONTACT_3_PIN};
        
        for (int i = 0; i < 3; i++) {
            // Đọc level GPIO: 0 (GND) = Contact Đóng, 1 = Contact Mở
            int raw_level = gpio_get_level(pins[i]);
            int new_state = (raw_level == 0) ? 1 : 0;  // Đảo logic cho pull-up
            
            // Kiểm tra sự thay đổi
            if (new_state != contact_states[i].current_state) {
                contact_states[i].debounce_count++;
                
                // Nếu state ổn định > threshold thời gian, xác nhận thay đổi
                if (contact_states[i].debounce_count >= DEBOUNCE_THRESHOLD) {
                    int old_state = contact_states[i].current_state;
                    contact_states[i].current_state = new_state;
                    contact_states[i].previous_state = old_state;
                    contact_states[i].last_change_time = esp_timer_get_time() / 1000;
                    contact_states[i].debounce_count = 0;
                    
                    // Ghi log sự thay đổi
                    const char *state_str = new_state ? "ĐÓNG" : "MỞ";
                    ESP_LOGI(TAG, "Contact %d state changed to: %s", i + 1, state_str);
                    
                    // Gọi callback nếu đã đăng ký
                    if (user_callback != NULL) {
                        user_callback(i + 1, new_state);
                    }
                }
            } else {
                contact_states[i].debounce_count = 0;
            }
        }
    }
}

//quản lý giám sát trạng thái liên tục
void dry_contact_start_monitor(void) {
    if (contact_task_handle == NULL) {
        xTaskCreate(
            dry_contact_monitor_task,    // Task function
            "dry_contact_monitor",       // Task name
            2048,                        // Stack size
            NULL,                        // Parameter
            5,                           // Priority
            &contact_task_handle         // Task handle
        );
        ESP_LOGI(TAG, "Dry Contact Monitor Task created");
    }
}

// dừng task giám sát trạng thái
void dry_contact_stop_task(void) {
    if (contact_task_handle != NULL) {
        vTaskDelete(contact_task_handle);
        contact_task_handle = NULL;
        ESP_LOGI(TAG, "Dry Contact Monitor Task deleted");
    }
}

// Status Function
void dry_contact_print_status(void) {
    ESP_LOGI(TAG, "=== Dry Contact Status ===");
    for (int i = 0; i < 3; i++) {
        const char *state = contact_states[i].current_state ? "ĐÓNG" : "MỞ";
        ESP_LOGI(TAG, "Contact %d: %s (Last change: %lld ms)", 
                 i + 1, state, contact_states[i].last_change_time);
    }
}

