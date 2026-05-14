#include "sht35.h"
#include "modbusSRC.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include <string.h>

static uint8_t getValue[8] = {0x01,0x03,0x00,0x00,0x00,0x02,0x00,0x00};

void SHT35_init(SHT35 *dev, uart_port_t uart, uint8_t addr)
{
    dev->uart = uart;
    // dev->de_re_pin = de_re;
    dev->addr = addr;
    dev->timeout = 1000;
}

void SHT35_begin(SHT35 *dev)
{
    // gpio_set_direction(dev->de_re_pin, GPIO_MODE_OUTPUT);
    // rs485_tx(dev, 0);
}

void SHT35_setTimeout(SHT35 *dev, uint16_t timeOut)
{
    if (timeOut < MIN_SHT35_TIMEOUT)
        dev->timeout = MIN_SHT35_TIMEOUT;
    else if (timeOut > MAX_SHT35_TIMEOUT)
        dev->timeout = MAX_SHT35_TIMEOUT;
    else
        dev->timeout = timeOut;
}

dataSHT35 SHT35_getData(SHT35 *dev)
{
    dataSHT35 value = {0};
    value.is_valid = 0;  // Mặc định: dữ liệu không hợp lệ

    uart_flush(dev->uart);

    getValue[0] = dev->addr;
    uint16_t crc = checkModbusCRC(getValue, 6);
    getValue[6] = crc & 0xFF;
    getValue[7] = crc >> 8;

    uart_flush_input(dev->uart);

    // rs485_tx(dev, 1);
    uart_write_bytes(dev->uart, getValue, 8);
    uart_wait_tx_done(dev->uart, pdMS_TO_TICKS(20));
    // rs485_tx(dev, 0);

    uint8_t rx[9];
    uint64_t start = esp_timer_get_time() / 1000;

    while ((esp_timer_get_time() / 1000 - start) < dev->timeout)
    {
        int len = uart_read_bytes(dev->uart, rx, 9, pdMS_TO_TICKS(20));
        if (len == 9)
        {
            if (checkModbusCRC(rx, 9) == 0)
            {
                int16_t temp = (rx[5] << 8) | rx[6];
                value.temperatureC = temp / 10.0f;
                value.temperatureF = value.temperatureC * 1.8f + 32;
                value.humidity = ((rx[3] << 8) | rx[4]) / 10.0f;
                value.is_valid = 1;  // Dữ liệu hợp lệ
            }
            break;
        }
    }
    return value;
}

float SHT35_readTemperature(SHT35 *dev, int isCelsius)
{
    dataSHT35 v = SHT35_getData(dev);
    return isCelsius ? v.temperatureC : v.temperatureF;
}

float SHT35_readHumidity(SHT35 *dev)
{
    return SHT35_getData(dev).humidity;
}

void SHT35_ChangeID(SHT35 *dev, uint8_t old_id, uint8_t new_id)
{
    uint8_t cmd[8];
    
    // Frame structure: [Slave_ID][Function_Code][Reg_Addr_H][Reg_Addr_L][Value_H][Value_L][CRC_L][CRC_H]
    cmd[0] = old_id;         // Current slave ID
    cmd[1] = 0x06;           // Modbus Function Code 06: Write Single Register
    cmd[2] = 0x01;           // Register address (High byte) - typically 0x0101
    cmd[3] = 0x01;           // Register address (Low byte)
    cmd[4] = 0x00;           // Value (High byte)
    cmd[5] = new_id;         // Value (Low byte) - new ID to write
    
    // Calculate and append CRC-16
    uint16_t crc = checkModbusCRC(cmd, 6);
    cmd[6] = crc & 0xFF;     // CRC Low byte
    cmd[7] = crc >> 8;       // CRC High byte
    
    // Clear buffer and send command
    uart_flush_input(dev->uart);
    uart_write_bytes(dev->uart, cmd, 8);
    uart_wait_tx_done(dev->uart, pdMS_TO_TICKS(100));
    
    // Note: Sensor will typically echo the response and restart.
    // Allow time for device to reinitialize (1-2 seconds)
}
