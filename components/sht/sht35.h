#pragma once
#include <stdint.h>
#include "driver/uart.h"
#include "driver/gpio.h"

#define MIN_SHT35_TIMEOUT 100
#define MAX_SHT35_TIMEOUT 3000

typedef struct
{
    float temperatureC;
    float temperatureF;
    float humidity;
    uint8_t is_valid;  // 1 = dữ liệu hợp lệ, 0 = không hợp lệ
} dataSHT35;

typedef struct
{
    uart_port_t uart;
    uint8_t     addr;
    uint16_t    timeout;
} SHT35;

void SHT35_init(SHT35 *dev, uart_port_t uart, uint8_t addr);

void SHT35_begin(SHT35 *dev);
void SHT35_setTimeout(SHT35 *dev, uint16_t timeout);
dataSHT35 SHT35_getData(SHT35 *dev);
float SHT35_readTemperature(SHT35 *dev, int isCelsius);
float SHT35_readHumidity(SHT35 *dev);

// Change Slave ID command (Modbus RTU Function Code 06 - Write Single Register)
void SHT35_ChangeID(SHT35 *dev, uint8_t old_id, uint8_t new_id);
