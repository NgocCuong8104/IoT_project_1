#include "web.h"
#include "relay.h" 
#include "esp_http_server.h"
#include "esp_log.h"
#include "cJSON.h" 

static const char *TAG = "WEB";

static const char *html_page = 
"<!DOCTYPE html><html>"
"<head><meta charset='UTF-8'><meta name='viewport' content='width=device-width, initial-scale=1'>"
"<style>"
"body { font-family: Arial; text-align: center; background: #f2f2f2; margin: 0; padding: 20px; }"
".container { max-width: 400px; margin: auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 4px 8px rgba(0,0,0,0.1); }"
"h2 { color: #333; }"
".grid { display: grid; grid-template-columns: 1fr 1fr; gap: 15px; margin-top: 20px; }"
".btn { padding: 20px; font-size: 18px; border: none; border-radius: 8px; cursor: pointer; transition: 0.3s; color: white; }"
".on { background-color: #2ecc71; box-shadow: 0 4px #27ae60; }"
".off { background-color: #95a5a6; box-shadow: 0 4px #7f8c8d; }"
".btn:active { transform: translateY(4px); box-shadow: none; }"
".status { margin-top: 20px; font-size: 14px; color: #666; }"
"</style></head>"
"<body>"
"<div class='container'>"
"  <h2>ĐIỀU KHIỂN THIẾT BỊ</h2>"
"  <div class='grid'>"
"    <button id='btn1' class='btn off' onclick='toggle(1)'>ĐÈN 1</button>"
"    <button id='btn2' class='btn off' onclick='toggle(2)'>ĐÈN 2</button>"
"    <button id='btn3' class='btn off' onclick='toggle(3)'>ĐÈN 3</button>"
"    <button id='btn4' class='btn off' onclick='toggle(4)'>ĐÈN 4</button>"
"    <button id='btn5' class='btn off' onclick='toggle(5)'>ĐÈN 5</button>"
"  </div>"
"  <p class='status'>Trạng thái tự cập nhật mỗi 1s</p>"
"</div>"

"<script>"
"function updateUI(states) {"
"  for(let i=0; i<5; i++) {"
"    let btn = document.getElementById('btn'+(i+1));"
"    if(states[i] == 1) { btn.className = 'btn on'; }"
"    else { btn.className = 'btn off'; }"
"  }"
"}"

"function toggle(id) {"
"  fetch('/api/toggle?id=' + id).then(response => getStatus());"
"}"

"function getStatus() {"
"  fetch('/api/status')"
"    .then(response => response.json())"
"    .then(data => updateUI(data.relays));"
"}"

"// Tự động cập nhật trạng thái mỗi 1 giây (để đồng bộ với nút cứng)"
"setInterval(getStatus, 1000);"
"getStatus(); // Gọi ngay khi load trang"
"</script>"
"</body></html>";

static esp_err_t root_handler(httpd_req_t *req) {
    httpd_resp_send(req, html_page, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req) {
    int states[5];
    for(int i=0; i<5; i++) {
        states[i] = relay_get_status(i+1); 
    }

    char json_resp[100];
    sprintf(json_resp, "{\"relays\": [%d, %d, %d, %d, %d]}", 
            states[0], states[1], states[2], states[3], states[4]);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_resp, strlen(json_resp));
    return ESP_OK;
}

static esp_err_t toggle_handler(httpd_req_t *req) {
    char buf[10];
    char param[10];
    
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        if (httpd_query_key_value(buf, "id", param, sizeof(param)) == ESP_OK) {
            int id = atoi(param);
            if (id >= 1 && id <= 5) {
                ESP_LOGI(TAG, "Web request toggle Relay %d", id);
                relay_toggle(id); 
            }
        }
    }
    
    httpd_resp_send(req, "OK", 2); 
    return ESP_OK;
}

void webserver_init(void) {
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 10; 
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t root = { .uri = "/", .method = HTTP_GET, .handler = root_handler };
        httpd_register_uri_handler(server, &root);

        httpd_uri_t status = { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler };
        httpd_register_uri_handler(server, &status);

        httpd_uri_t toggle = { .uri = "/api/toggle", .method = HTTP_GET, .handler = toggle_handler };
        httpd_register_uri_handler(server, &toggle);
        
        ESP_LOGI(TAG, "Webserver Dashboard started");
    }
}