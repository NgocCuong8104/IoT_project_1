#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "esp_log.h"
#include "mqtt_client.h"
#include "mqtt.h"
#include "relay.h"            
#include "wifi.h"
#include "scheduler.h"
#include "cJSON.h"
#include "platform.h"
#include "ota.h"
#include <sys/time.h>
#include <esp_ota_ops.h>
#include "input.h"
#include "esp_crt_bundle.h"

static const char *TAG = "MQTT_COMPONENT";

static esp_mqtt_client_handle_t client = NULL; 
static bool mqtt_connected = false;
static char lwm_topic[64]; 

extern float current_temp;
extern float current_hum;
extern uint8_t sensor_valid;

// #define MAX_RELAYS 5
#define DEVICE_MODEL "ESP32_v1"

static long get_timestamp(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec;
}

// Thông tin thiết bị 
static void publish_device_capabilities(esp_mqtt_client_handle_t mqtt_client) {
    char topic[128];
    snprintf(topic, sizeof(topic), "/device/%s/info", platform_get_id());

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", DEVICE_MODEL);
    cJSON_AddNumberToObject(root, "relay", NUM_RELAYS);
    cJSON_AddStringToObject(root, "firmware_version", platform_get_version());

    char *payload = cJSON_PrintUnformatted(root);
   
    esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 1);
    ESP_LOGI(TAG, "dữ liệu thông tin thiết bị: %s", payload);

    cJSON_Delete(root);
    free(payload);
}

// hàm gửi phản hồi trạng thái relay và trạng thái thiết bị qua MQTT
void mqtt_send_response(int relay_idx, int state) {
    if (!mqtt_is_connected()) return; 

    char topic[128];
    char *id = platform_get_id();
    snprintf(topic, sizeof(topic), "/device/%s/response", id);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "relay", relay_idx);
    cJSON_AddNumberToObject(root, "state", state);
    cJSON_AddNumberToObject(root, "timestamp", get_timestamp());

    char *payload = cJSON_PrintUnformatted(root);
    
    esp_mqtt_client_publish(client, topic, payload, 0, 0, 0);
    ESP_LOGI(TAG, "Sent Response: %s -> %s", topic, payload);

    cJSON_Delete(root); 
    free(payload);      
}

// Hàm giúp log lỗi chi tiết khi có lỗi MQTT xảy ra
static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

// Hàm gửi trạng thái thiết bị hiện tại qua MQTT, bao gồm trạng thái relay, cảm biến, v.v.
void mqtt_send_status(void) {
    if (!mqtt_is_connected()) return;

    char topic[128];
    char *id = platform_get_id();
    snprintf(topic, sizeof(topic), "/device/%s/status", id);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_id", id);
    cJSON_AddNumberToObject(root, "status", 1); // 1 = online, 0 = offline

    cJSON *relays_arr = cJSON_CreateArray();
    for (int i = 1; i <= NUM_RELAYS; i++) {
        cJSON_AddItemToArray(relays_arr, cJSON_CreateNumber(relay_get_status(i)));
    }
    cJSON_AddItemToObject(root, "relays", relays_arr);

    // cJSON *inputs_arr = cJSON_CreateIntArray((const int[]){0, 1, 0}, 3);

    int really_inputs[3];
    really_inputs[0] = dry_contact_read_state(1);
    really_inputs[1] = dry_contact_read_state(2);
    really_inputs[2] = dry_contact_read_state(3);

    cJSON *inputs_arr = cJSON_CreateIntArray(really_inputs, 3);
    cJSON_AddItemToObject(root, "inputs", inputs_arr);

    // Bảo vệ khi đọc sensor data
    sensor_data_lock();
    uint8_t local_sensor_valid = sensor_valid;
    float local_current_temp = current_temp;
    float local_current_hum = current_hum;
    sensor_data_unlock();

    if (local_sensor_valid) {
        cJSON_AddNumberToObject(root, "temp", local_current_temp);
        cJSON_AddNumberToObject(root, "hum", local_current_hum);
    } else {
        cJSON_AddNumberToObject(root, "temp", 0);
        cJSON_AddNumberToObject(root, "hum", 0);
    }

    cJSON_AddNumberToObject(root, "timestamp", get_timestamp());

    char *payload = cJSON_PrintUnformatted(root);
    esp_mqtt_client_publish(client, topic, payload, 0, 0, 1);

    cJSON_Delete(root);
    free(payload);
}

// hàm gửi trạng thái relay sau khi thay đổi và cập nhật trạng thái thiết bị qua MQTT
void mqtt_send_state(int relay_idx, int state) {
    mqtt_send_response(relay_idx, state);
    mqtt_send_status();
    ESP_LOGI(TAG, "State updated for relay %d: %d", relay_idx, state);
}

// hàm xử lý sự kiện MQTT
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        mqtt_connected = true;
        
        ESP_LOGI(TAG, "Connected! Free Heap: %lu bytes", esp_get_free_heap_size());
        
        ota_set_mqtt_client(event->client);
        
        publish_device_capabilities(event->client);

        // đăng ký subscribe cho các topic cần thiết sau khi kết nối thành công
        char topic_sub[128];
        char *id = platform_get_id();
        snprintf(topic_sub, sizeof(topic_sub), "/server/%s/#", id);

        esp_mqtt_client_subscribe(event->client, topic_sub, 0);
        ESP_LOGI(TAG, "Subscribed to %s", topic_sub);

        mqtt_send_status();
        break;
    }

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED - Will attempt auto-reconnect");
        mqtt_connected = false;
        ESP_LOGI(TAG, "Free Heap: %lu bytes", esp_get_free_heap_size());
        break;

    case MQTT_EVENT_SUBSCRIBED:
    case MQTT_EVENT_UNSUBSCRIBED:
    case MQTT_EVENT_PUBLISHED:
        break;

    case MQTT_EVENT_DATA: {
        ESP_LOGI(TAG, "MQTT_EVENT_DATA");
        
        char *data_buf = malloc(event->data_len + 1);
        if (data_buf == NULL) break;
        memcpy(data_buf, event->data, event->data_len);
        data_buf[event->data_len] = '\0';

        char *topic_buf = malloc(event->topic_len + 1);
        if (topic_buf == NULL) {
            free(data_buf);
            break;
        }
        memcpy(topic_buf, event->topic, event->topic_len);
        topic_buf[event->topic_len] = '\0';
        
        // Log chi tiết topic và payload nhận được
        char *dev_id = platform_get_id();
        char cmd_topic[128];
        char cfg_topic[128];
        char ota_topic[128];

        // snprintf để tạo các topic dựa trên device ID
        snprintf(cmd_topic, sizeof(cmd_topic), "/server/%s/command", dev_id);
        snprintf(cfg_topic, sizeof(cfg_topic), "/server/%s/config", dev_id);
        snprintf(ota_topic, sizeof(ota_topic), "/server/%s/ota", dev_id);

        // Nếu topic nhận được là topic OTA, xử lý OTA trigger
        if (strcmp(topic_buf, ota_topic) == 0) {
            ESP_LOGI(TAG, "[OTA MQTT] OTA command received");
            
            cJSON *root = cJSON_Parse(data_buf);
            if (root != NULL) {
                // kiểm tra model trước khi xử lý OTA trigger để đảm bảo chỉ các lệnh OTA dành cho model này mới được chấp nhận
                cJSON *model = cJSON_GetObjectItem(root, "model");
                if (model && cJSON_IsString(model)) {
                    if (strcmp(model->valuestring, DEVICE_MODEL) != 0) {
                        ESP_LOGW(TAG, "[OTA MQTT] Model mismatch - expected: %s, got: %s", 
                                 DEVICE_MODEL, model->valuestring);
                        cJSON_Delete(root);
                        free(data_buf);
                        free(topic_buf);
                        break;
                    }
                }
                
                // Xử lý OTA trigger nếu model khớp hoặc không có trường model
                handle_ota_trigger(root);
                
                // 3. Giải phóng bộ nhớ sau khi xử lý xong
                cJSON_Delete(root);
            } else {
                ESP_LOGE(TAG, "[OTA MQTT] Failed to parse OTA JSON");
            }
        }

        // Nếu topic nhận được là topic command, xử lý lệnh điều khiển relay
        else if (strcmp(topic_buf, cmd_topic) == 0) {
            cJSON *root = cJSON_Parse(data_buf);
            if (root != NULL) {
                cJSON *relay_item = cJSON_GetObjectItem(root, "relay");
                cJSON *val_item = cJSON_GetObjectItem(root, "value");

                if (cJSON_IsNumber(relay_item) && cJSON_IsNumber(val_item)) {
                    int relay_idx = relay_item->valueint; 
                    int val = val_item->valueint;
                    if (relay_idx > 0 && relay_idx <= NUM_RELAYS) {
                        relay_set(relay_idx, val, true);
                        mqtt_send_state(relay_idx, val);
                    }
                }
                cJSON_Delete(root);
            } else {
                ESP_LOGW(TAG, "Failed to parse command JSON");
            }
        }
        else if (strcmp(topic_buf, cfg_topic) == 0) {
            scheduler_update_from_json(data_buf);
        } else {
            ESP_LOGW(TAG, "Unknown topic received: %s", topic_buf);
        }

        free(data_buf);
        free(topic_buf);
        break;
    }

    // xử lý lỗi mqtt
    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
        mqtt_connected = false; 
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno", event->error_handle->esp_transport_sock_errno);
        }
        ESP_LOGI(TAG, "Will auto-reconnect after error. Free Heap: %lu bytes", esp_get_free_heap_size());
        break;

    default:
        break;
    }
}

void mqtt_init(void)
{
    if (client != NULL) {
        esp_mqtt_client_stop(client);
        esp_mqtt_client_destroy(client);
        client = NULL;
    }

    char *unique_id = platform_get_id();
    ESP_LOGI(TAG, "Khoi tao MQTT voi Unique Client ID: %s", unique_id);

    snprintf(lwm_topic, sizeof(lwm_topic), "/device/%s/status", unique_id);

    static char lwm_payload[64]; 
    snprintf(lwm_payload, sizeof(lwm_payload), "{\"device_id\":\"%s\",\"status\":0}", unique_id);
    
    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "wss://iot.croptex.io:443/mqtt",
        .session.protocol_ver = MQTT_PROTOCOL_V_3_1_1,
        .network.disable_auto_reconnect = false,
        .broker.verification.crt_bundle_attach = esp_crt_bundle_attach,
        // .credentials.username = "acceleration",
        // .credentials.authentication.password = "2O2bkHOEMVm37hHNvqe7",
        
        .session.last_will.topic = lwm_topic,
        .session.last_will.msg = lwm_payload,
        .session.keepalive = 60,
        .session.last_will.msg_len = strlen(lwm_payload),
        .session.last_will.qos = 1,
        .session.last_will.retain = true,
        
        .credentials.client_id = unique_id,
    };

    client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(client);
}
bool mqtt_is_connected(void) {
    return (client != NULL) && mqtt_connected;
}

void mqtt_publish(const char *topic, const char *payload) {
    if (!mqtt_is_connected()) {
        ESP_LOGW(TAG, "MQTT not connected, cannot publish to %s", topic);
        return;
    }
    
    int msg_id = esp_mqtt_client_publish(client, topic, payload, 0, 0, 0);
    if (msg_id >= 0) {
        ESP_LOGI(TAG, "Published to %s, msg_id=%d", topic, msg_id);
    } else {
        ESP_LOGW(TAG, "Failed to publish to %s, error=%d", topic, msg_id);
    }
}