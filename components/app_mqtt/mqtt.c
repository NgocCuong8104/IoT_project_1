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

static const char *TAG = "MQTT_COMPONENT";

static esp_mqtt_client_handle_t client = NULL; 
static bool mqtt_connected = false;
static char lwm_topic[64]; 

extern float current_temp;
extern float current_hum;

#define MAX_RELAYS 5
#define DEVICE_MODEL "ESP32_v1"

static long get_timestamp(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec;
}

static void publish_device_capabilities(esp_mqtt_client_handle_t mqtt_client) {
    char topic[128];
    snprintf(topic, sizeof(topic), "/device/%s/info", platform_get_id());

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "model", DEVICE_MODEL);
    cJSON_AddNumberToObject(root, "max_relays", MAX_RELAYS);
    cJSON_AddStringToObject(root, "firmware_version", platform_get_version());

    char *payload = cJSON_PrintUnformatted(root);
   
    esp_mqtt_client_publish(mqtt_client, topic, payload, 0, 1, 1);
    ESP_LOGI(TAG, "Published Device Info (Retain): %s", payload);

    cJSON_Delete(root);
    free(payload);
}

void mqtt_send_response(int relay_idx, int state) {
    if (client == NULL || !mqtt_connected) return;

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

static void log_error_if_nonzero(const char *message, int error_code)
{
    if (error_code != 0) {
        ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
    }
}

void mqtt_send_status(void) {
    if (client == NULL || !mqtt_connected) return;

    char topic[128];
    char *id = platform_get_id();
    snprintf(topic, sizeof(topic), "/device/%s/status", id);

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "device_id", id);
    cJSON_AddNumberToObject(root, "status", 1);

    // Tự động mảng dựa vào MAX_RELAYS
    cJSON *relays_arr = cJSON_CreateArray();
    for (int i = 1; i <= MAX_RELAYS; i++) {
        cJSON_AddItemToArray(relays_arr, cJSON_CreateNumber(relay_get_status(i)));
    }
    cJSON_AddItemToObject(root, "relays", relays_arr);

    cJSON *inputs_arr = cJSON_CreateIntArray((const int[]){0, 1, 0}, 3);
    cJSON_AddItemToObject(root, "inputs", inputs_arr);

    cJSON_AddNumberToObject(root, "temp", current_temp);
    cJSON_AddNumberToObject(root, "hum", current_hum);

    cJSON_AddNumberToObject(root, "timestamp", get_timestamp());

    char *payload = cJSON_PrintUnformatted(root);
    esp_mqtt_client_publish(client, topic, payload, 0, 0, 1);

    cJSON_Delete(root);
    free(payload);
}

void mqtt_send_state(int relay_idx, int state) {
    mqtt_send_response(relay_idx, state);
    mqtt_send_status();
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED: {
        ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");
        mqtt_connected = true;
        
        ota_set_mqtt_client(event->client);
        
        // 1. Gửi thông tin số lượng relay cho app khi vừa kết nối
        publish_device_capabilities(event->client);

        char topic_sub[128];
        char *id = platform_get_id();
        snprintf(topic_sub, sizeof(topic_sub), "/server/%s/#", id);

        esp_mqtt_client_subscribe(event->client, topic_sub, 0);
        ESP_LOGI(TAG, "Subscribed to %s", topic_sub);

        // 2. Gửi trạng thái ON/OFF hiện tại
        mqtt_send_status();
        break;
    }

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");
        mqtt_connected = false;
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
        
        char *dev_id = platform_get_id();
        char cmd_topic[128];
        char cfg_topic[128];
        char ota_topic[128];

        snprintf(cmd_topic, sizeof(cmd_topic), "/server/%s/command", dev_id);
        snprintf(cfg_topic, sizeof(cfg_topic), "/server/%s/config", dev_id);
        snprintf(ota_topic, sizeof(ota_topic), "/server/%s/ota", dev_id);

        // ========== OTA COMMAND ==========
        if (strcmp(topic_buf, ota_topic) == 0) {
            ESP_LOGI(TAG, "OTA Command Received");
            cJSON *root = cJSON_Parse(data_buf);
            if (root != NULL) {
                cJSON *action = cJSON_GetObjectItem(root, "action");
                if (action && cJSON_IsString(action)) {
                    if (strcmp(action->valuestring, "update") == 0) {
                        cJSON *url = cJSON_GetObjectItem(root, "url");
                        cJSON *model = cJSON_GetObjectItem(root, "model");
                        cJSON *version = cJSON_GetObjectItem(root, "version");
                        
                        // Kiểm tra model (nếu có)
                        if (model && cJSON_IsString(model)) {
                            if (strcmp(model->valuestring, DEVICE_MODEL) != 0) {
                                ESP_LOGW(TAG, "OTA rejected: model mismatch (expected: %s, got: %s)", 
                                         DEVICE_MODEL, model->valuestring);
                                cJSON_Delete(root);
                                free(data_buf);
                                free(topic_buf);
                                break;
                            }
                        }
                        
                        // Log version nếu có
                        if (version && cJSON_IsString(version)) {
                            ESP_LOGI(TAG, "OTA target version: %s", version->valuestring);
                        }
                        
                        if (url && cJSON_IsString(url)) {
                            ota_start(url->valuestring);
                        }
                    }
                }
                cJSON_Delete(root);
            }
        }
        // ========== RELAY COMMAND ==========
        else if (strcmp(topic_buf, cmd_topic) == 0) {
            cJSON *root = cJSON_Parse(data_buf);
            if (root != NULL) {
                cJSON *relay_item = cJSON_GetObjectItem(root, "relay");
                cJSON *val_item = cJSON_GetObjectItem(root, "value");

                if (cJSON_IsNumber(relay_item) && cJSON_IsNumber(val_item)) {
                    int relay_idx = relay_item->valueint;
                    int val = val_item->valueint;
                    // Lọc nhiễu: Chỉ cho phép điều khiển relay trong phạm vi cho phép
                    if (relay_idx > 0 && relay_idx <= MAX_RELAYS) {
                        relay_set(relay_idx, val);
                        mqtt_send_state(relay_idx, val);
                    }
                }
                cJSON_Delete(root);
            }
        }
        // ========== CONFIG COMMAND ==========
        else if (strcmp(topic_buf, cfg_topic) == 0) {
            scheduler_update_from_json(data_buf);
        }

        free(data_buf);
        free(topic_buf);
        break;
    }

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "MQTT_EVENT_ERROR");
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT) {
            log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
            log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
            log_error_if_nonzero("captured as transport's socket errno", event->error_handle->esp_transport_sock_errno);
        }
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
        // .broker.address.hostname = "192.168.1.251",
        // .broker.address.port = 1883,
        // .broker.address.transport = MQTT_TRANSPORT_OVER_TCP,
        .broker.address.uri = "mqtt://136.110.45.16:1883",
        .session.protocol_ver = MQTT_PROTOCOL_V_3_1_1,
        .network.disable_auto_reconnect = false,
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

void mqtt_publish(const char *topic, const char *payload) {
    if (client != NULL) {
        int msg_id = esp_mqtt_client_publish(client, topic, payload, 0, 0, 0);
        ESP_LOGI(TAG, "Published to %s, msg_id=%d", topic, msg_id);
    } else {
        ESP_LOGE(TAG, "MQTT Client not initialized, cannot publish");
        ESP_LOGI(TAG, "Attempted to publish: %s -> %s", topic, payload);
    }
}