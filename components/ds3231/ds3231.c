#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "ds3231.h"

static const char *TAG = "DS3231";
static i2c_master_bus_handle_t bus_handle = NULL;
static i2c_master_dev_handle_t dev_handle = NULL;
static int ds3231_initialized = 0;


static esp_err_t ds3231_read_register(uint8_t reg, uint8_t *value); // khai báo hàm đọc 1 thanh ghi

static uint8_t bcd_to_bin(uint8_t bcd) {
    return ((bcd >> 4) * 10) + (bcd & 0x0F); // chuyển BCD sang nhị phân
}

static uint8_t bin_to_bcd(uint8_t bin) {
    return ((bin / 10) << 4) | (bin % 10);
}

esp_err_t ds3231_init(uint8_t sda_pin, uint8_t scl_pin, uint32_t i2c_freq) {
    if (ds3231_initialized) {
        ESP_LOGW(TAG, "DS3231 already initialized");
        return ESP_OK;
    }

    esp_err_t ret = ESP_OK;

    // cài đặt cấu hình I2C master bus
    i2c_master_bus_config_t i2c_bus_config = {
        .i2c_port = -1,
        .sda_io_num = sda_pin,
        .scl_io_num = scl_pin,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0, 
        .trans_queue_depth = 0,
        .flags = {
            .enable_internal_pullup = true,
        },
    };

    ret = i2c_new_master_bus(&i2c_bus_config, &bus_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create I2C bus: %s", esp_err_to_name(ret));
        return ret;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = DS3231_ADDR,
        .scl_speed_hz = i2c_freq,
    };

    ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &dev_handle); // thêm DS3231 làm thiết bị trên bus I2C (địa chỉ 0x68, tốc độ 100kHz, tên thiết bị "ds3231")
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add DS3231 device: %s", esp_err_to_name(ret));
        i2c_del_master_bus(bus_handle);
        bus_handle = NULL;
        return ret;
    }

    ds3231_initialized = 1;
    ESP_LOGI(TAG, "DS3231 I2C device added successfully");

    // kiểm tra kết nối bằng cách đọc thanh ghi trạng thái
    uint8_t status = 0;
    ret = ds3231_read_register(DS3231_REG_STATUS, &status);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FAILED TO DETECT DS3231! Status register read failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "DS3231 CONNECTION VERIFIED on SDA=%d, SCL=%d, Freq=%lu Hz, Status=0x%02X", 
             sda_pin, scl_pin, (unsigned long)i2c_freq, status);

    return ESP_OK;
}

void ds3231_deinit(void) {
    if (dev_handle != NULL) {
        i2c_master_bus_rm_device(dev_handle); 
        dev_handle = NULL;
    }
    if (bus_handle != NULL) {
        i2c_del_master_bus(bus_handle);
        bus_handle = NULL;
    }
    ds3231_initialized = 0;
    ESP_LOGI(TAG, "DS3231 deinitialized");
}

int ds3231_is_init(void) {
    return ds3231_initialized;
}

static esp_err_t ds3231_read_register(uint8_t reg, uint8_t *value) {
    if (!ds3231_initialized) {
        ESP_LOGE(TAG, "DS3231 not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    return i2c_master_transmit_receive(dev_handle, &reg, 1, value, 1, -1); // gửi 1 byte (địa chỉ thanh ghi) và đọc 1 byte dữ liệu trả về
}

static esp_err_t ds3231_read_registers(uint8_t start_reg, uint8_t *data, uint8_t len) {
    if (!ds3231_initialized) {
        ESP_LOGE(TAG, "DS3231 not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    return i2c_master_transmit_receive(dev_handle, &start_reg, 1, data, len, -1);
}

esp_err_t ds3231_read_time_struct(struct tm *timeinfo) {
    if (!ds3231_initialized) {
        ESP_LOGE(TAG, "DS3231 not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (timeinfo == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t data[7];
    esp_err_t ret = ds3231_read_registers(DS3231_REG_SECONDS, data, 7);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read time from DS3231: %s", esp_err_to_name(ret));
        return ret;
    }

    timeinfo->tm_sec = bcd_to_bin(data[0] & 0x7F);
    timeinfo->tm_min = bcd_to_bin(data[1] & 0x7F);
    timeinfo->tm_hour = bcd_to_bin(data[2] & 0x3F);
    timeinfo->tm_mday = bcd_to_bin(data[4] & 0x3F);
    timeinfo->tm_mon = bcd_to_bin(data[5] & 0x1F) - 1;
    timeinfo->tm_year = bcd_to_bin(data[6]) + 100;
    timeinfo->tm_wday = bcd_to_bin(data[3]) - 1;

    ESP_LOGD(TAG, "Time read from DS3231: %04d-%02d-%02d %02d:%02d:%02d (dow=%u)",
             timeinfo->tm_year + 1900, timeinfo->tm_mon + 1, timeinfo->tm_mday,
             timeinfo->tm_hour, timeinfo->tm_min, timeinfo->tm_sec, timeinfo->tm_wday);

    return ESP_OK;
}

esp_err_t ds3231_write_time_struct(const struct tm *timeinfo) {
    if (!ds3231_initialized) {
        ESP_LOGE(TAG, "DS3231 not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (timeinfo == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t write_buf[8] = {
        DS3231_REG_SECONDS,
        bin_to_bcd(timeinfo->tm_sec),
        bin_to_bcd(timeinfo->tm_min),
        bin_to_bcd(timeinfo->tm_hour),
        bin_to_bcd(timeinfo->tm_wday + 1),
        bin_to_bcd(timeinfo->tm_mday),
        bin_to_bcd(timeinfo->tm_mon + 1),
        bin_to_bcd(timeinfo->tm_year - 100)
    };

    esp_err_t ret = i2c_master_transmit(dev_handle, write_buf, 8, -1);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write time to DS3231: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Time written to DS3231 successfully");
    }
    return ret;
}

esp_err_t ds3231_read_time(time_t *time) {
    if (time == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    struct tm timeinfo = {0};
    esp_err_t ret = ds3231_read_time_struct(&timeinfo);
    if (ret != ESP_OK) {
        return ret;
    }

    *time = mktime(&timeinfo);
    return ESP_OK;
}

esp_err_t ds3231_write_time(time_t time) {
    struct tm *timeinfo = localtime(&time);
    return ds3231_write_time_struct(timeinfo);
}

float ds3231_read_temperature(void) {
    if (!ds3231_initialized) {
        ESP_LOGE(TAG, "DS3231 not initialized");
        return 0.0f;
    }

    uint8_t temp_data[2] = {0};
    if (ds3231_read_registers(DS3231_REG_TEMP_MSB, temp_data, 2) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read temperature");
        return 0.0f;
    }

    float temperature = (float)temp_data[0] + ((temp_data[1] >> 6) * 0.25f);
    ESP_LOGD(TAG, "✓ Temperature read from DS3231: %.2f°C", temperature);
    return temperature;
}

esp_err_t ds3231_test_connection(void) {
    if (!ds3231_initialized) {
        ESP_LOGE(TAG, "DS3231 not initialized - call ds3231_init() first");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "DS3231 Connection Diagnostic Test");

    // Test 1: Read Status Register
    ESP_LOGI(TAG, "Reading status register...");
    uint8_t status = 0;
    esp_err_t ret = ds3231_read_register(DS3231_REG_STATUS, &status);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FAILED: Status register read failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Status register: 0x%02X (OSF=%u, BSY=%u, CRSD=%u, EN32K=%u)",
             status,
             (status >> 7) & 1,  // nếu bằng 1 thì có lỗi đồng hồ, cần reset lại (thường do mất điện hoặc pin yếu)
             (status >> 2) & 1,  // nếu bằng 1 thì đang bận chuyển đổi nhiệt độ, không nên đọc nhiệt độ lúc này
             (status >> 1) & 1,  // Conversion flag
             status & 1);        // Enable 32kHz output

    // Test 2: Read Current Time
    ESP_LOGI(TAG, "Reading current time...");
    struct tm timeinfo = {0};
    ret = ds3231_read_time_struct(&timeinfo);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FAILED: Time read failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Current time: %04d-%02d-%02d %02d:%02d:%02d",
             timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
             timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);

    // Test 3: Read Temperature
    ESP_LOGI(TAG, "Reading temperature...");
    uint8_t temp_data[2] = {0};
    ret = ds3231_read_registers(DS3231_REG_TEMP_MSB, temp_data, 2);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Temperature read failed: %s", esp_err_to_name(ret));
        return ret;
    }
    float temp = (float)temp_data[0] + ((temp_data[1] >> 6) * 0.25f);
    ESP_LOGI(TAG, "Temperature: %.2f°C", temp);

    // Test 4: Read Control Register
    ESP_LOGI(TAG, "Reading control register...");
    uint8_t control = 0;
    ret = ds3231_read_register(DS3231_REG_CONTROL, &control);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "FAILED: Control register read failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "Control register: 0x%02X (EOSC=%u, BBSQW=%u, CONV=%u, RS=%u)",
             control,
             (control >> 7) & 1,  // EOSC - Enable Oscillator
             (control >> 6) & 1,  // BBSQW - Battery Backed SQW
             (control >> 5) & 1,  // CONV - Convert Temperature
             control & 3);        // RS - Rate Select

    ESP_LOGI(TAG, "DS3231 CONNECTION SUCCESSFUL");
    return ESP_OK;
}

i2c_master_bus_handle_t get_i2c_bus_handle(void) {
    return bus_handle;
}