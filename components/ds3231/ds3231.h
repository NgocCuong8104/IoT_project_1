#ifndef DS3231_H
#define DS3231_H

#include <stdint.h>
#include <time.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DS3231_ADDR           0x68 // Địa chỉ I2C của DS3231
#define DS3231_REG_SECONDS    0x00 // DS3231 sử dụng "seconds" để chỉ giây
#define DS3231_REG_MINUTES    0x01 // DS3231 sử dụng "minutes" để chỉ phút
#define DS3231_REG_HOURS      0x02 // DS3231 sử dụng "hours" để chỉ giờ
#define DS3231_REG_DAY        0x03 // DS3231 sử dụng "day" để chỉ ngày trong tuần (1-7)
#define DS3231_REG_DATE       0x04 // DS3231 sử dụng "date" để chỉ ngày trong tháng
#define DS3231_REG_MONTH      0x05 // DS3231 sử dụng "month" để chỉ tháng
#define DS3231_REG_YEAR       0x06 // DS3231 sử dụng "year" để chỉ năm
#define DS3231_REG_CONTROL    0x0E // DS3231 sử dụng "control" để điều khiển các chức năng
#define DS3231_REG_STATUS     0x0F // DS3231 sử dụng "status" để đọc trạng thái
#define DS3231_REG_TEMP_MSB   0x11 // DS3231 sử dụng "temp_msb" để đọc nhiệt độ MSB
#define DS3231_REG_TEMP_LSB   0x12 // DS3231 sử dụng "temp_lsb" để đọc nhiệt độ LSB

typedef struct {
    uint8_t sec;
    uint8_t min;
    uint8_t hour;
    uint8_t day;
    uint8_t date;
    uint8_t month;
    uint8_t year;
} ds3231_time_t;

esp_err_t ds3231_init(uint8_t sda_pin, uint8_t scl_pin, uint32_t i2c_freq);

esp_err_t ds3231_read_time(time_t *time);

esp_err_t ds3231_write_time(time_t time);

esp_err_t ds3231_read_time_struct(struct tm *timeinfo);

esp_err_t ds3231_write_time_struct(const struct tm *timeinfo);

float ds3231_read_temperature(void);

int ds3231_is_init(void);

void ds3231_deinit(void);

esp_err_t ds3231_test_connection(void);

#ifdef __cplusplus
}
#endif

#endif
