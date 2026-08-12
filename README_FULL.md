# 🌐 IoT_V1 - Giải Pháp IoT Mô-đun Hoàn Chỉnh

**Phiên bản**: 1.0.0  
**Nền tảng**: ESP-IDF 5.3.1  
**MCU**: ESP32  
**Trạng thái**: Production-Ready

---

## 📋 Mục Lục

1. [Tổng Quan](#tổng-quan)
2. [Tính Năng Chính](#tính-năng-chính)
3. [Yêu Cầu Hệ Thống](#yêu-cầu-hệ-thống)
4. [Cấu Trúc Dự Án](#cấu-trúc-dự-án)
5. [Hướng Dẫn Cài Đặt](#hướng-dẫn-cài-đặt)
6. [Hướng Dẫn Build](#hướng-dẫn-build)
7. [Cấu Hình Dự Án](#cấu-hình-dự-án)
8. [Lệnh Điều Khiển](#lệnh-điều-khiển)
9. [Kiến Trúc Hệ Thống](#kiến-trúc-hệ-thống)
10. [Pinout & GPIO Mapping](#pinout--gpio-mapping)
11. [NVS Storage](#nvs-storage)
12. [MQTT Topics](#mqtt-topics)
13. [Troubleshooting](#troubleshooting)

---

## 🎯 Tổng Quan

**IoT_V1** là một **giải pháp IoT mô-đun, sẵn sàng sản xuất** được xây dựng trên **ESP-IDF**. Nó cung cấp:

- ✅ Kết nối **WiFi + PPPoS (4G)** dự phòng
- ✅ Giao thức **MQTT** cho truyền dữ liệu thời gian thực
- ✅ Hỗ trợ **cảm biến SHT35/SHT40** qua Modbus RTU
- ✅ Điều khiển **5 relay + 5 nút bấm + 3 input khô**
- ✅ **RTC DS3231** cho dấu thời gian chính xác
- ✅ **OTA Update** - Cập nhật firmware không khí
- ✅ **Lập lịch nâng cao** - Schedule relay & sensor logic
- ✅ **Thread-safe** - Mutex bảo vệ dữ liệu công dùng

### Sơ Đồ Khối Chức Năng

```
┌──────────────────────────────────┐
│     ESP32 Main Controller        │
├──────────────────────────────────┤
│                                  │
│  ┌─ WiFi/PPPoS ─ MQTT ─ Cloud   │
│  │                               │
│  ├─ 5x Relay Control (GPIO)     │
│  ├─ 5x Button Input (GPIO)      │
│  ├─ 3x Dry Contact (GPIO)       │
│  ├─ SHT35 Temp/Humidity         │
│  ├─ DS3231 RTC                  │
│  ├─ LED Status Indicator         │
│  └─ NVS Flash Storage            │
│                                  │
└──────────────────────────────────┘
```

---

## ✨ Tính Năng Chính

### 🔌 Kết Nối

| Tính Năng | Mô Tả |
|-----------|-------|
| **WiFi STA** | Kết nối WiFi với BLE Provisioning |
| **PPPoS (4G)** | Kết nối dự phòng qua modem serial |
| **MQTT WSS** | Giao thức publish/subscribe bảo mật |
| **Auto Reconnect** | Tái kết nối tự động với backoff |

### 🎛️ Điều Khiển Thiết Bị

| Thiết Bị | Số Lượng | Chức Năng |
|----------|----------|----------|
| **Relay** | 5 | GPIO điều khiển, NVS persistent |
| **Button** | 5 | Giám sát nút bấm, debouncing 50ms |
| **Dry Contact** | 3 | Cảm biến tiếp xúc, logic action |
| **LED Status** | 1 | Chỉ báo trạng thái hệ thống |

### 📊 Cảm Biến & Đo Lường

| Cảm Biến | Giao Thức | Chức Năng |
|----------|-----------|----------|
| **SHT35** | Modbus RTU | Nhiệt độ & Độ ẩm |
| **DS3231** | I2C | RTC + NTP Sync |
| **Multiple** | Modbus RTU | Tối đa 10 cảm biến |

### ⏰ Lập Lịch & Tự Động Hóa

- **Lập lịch Relay**: Tối đa 10 khung giờ/relay, hỗ trợ ngày trong tuần
- **Logic Cảm biến**: Ngưỡng cao/thấp với hysteresis
- **Logic Input**: Điều khiển relay dựa trên trạng thái input
- **Persistent Storage**: Lưu toàn bộ cấu hình vào NVS

### 🚀 OTA Update

- **HTTPS Download** - Download firmware an toàn từ server
- **MD5 Verification** - Xác thực checksum
- **Atomic Update** - Flash vào partition ota_1
- **Rollback Protection** - Đánh dấu firmware hợp lệ sau boot
- **Progress Reporting** - Báo tiến độ qua MQTT

---

## 🔧 Yêu Cầu Hệ Thống

### Hardware
- **Mainboard**: ESP32 WROOM-32 (hoặc tương đương)
- **Memory**: Ít nhất 4MB Flash
- **Board**: Hỗ trợ USB-UART converter

### Software
- **ESP-IDF**: v5.3.1 trở lên
- **Python**: 3.8+
- **Git**: Để clone dependencies

### Môi Trường Phát Triển
```bash
# Cài đặt ESP-IDF
git clone --branch v5.3.1 https://github.com/espressif/esp-idf.git
cd esp-idf
./install.sh
```

---

## 📂 Cấu Trúc Dự Án

```
IoT_V1/
├── CMakeLists.txt                    # Cấu hình xây dựng project
├── README_FULL.md                    # Tài liệu này
├── partitions.csv                    # Sơ đồ phân chia flash
├── sdkconfig                         # Cấu hình ESP-IDF
├── sdkconfig.defaults                # Giá trị mặc định cấu hình
│
├── main/                             # Ứng dụng chính
│   ├── CMakeLists.txt
│   └── main.c                        # Điểm vào, khởi tạo & quản lý task
│
├── components/                       # Các thành phần mô-đun
│
│   ├── wifi/                         # Quản lý WiFi & BLE Provisioning
│   │   ├── wifi.h
│   │   ├── wifi.c
│   │   └── CMakeLists.txt
│   │
│   ├── app_mqtt/                     # MQTT Client & Pub/Sub
│   │   ├── mqtt.h
│   │   ├── mqtt.c
│   │   └── CMakeLists.txt
│   │
│   ├── pppos/                        # PPPoS (4G/LTE) Backup Connection
│   │   ├── pppos.h
│   │   ├── pppos.c
│   │   └── CMakeLists.txt
│   │
│   ├── relay/                        # Điều khiển 5 Relay
│   │   ├── relay.h
│   │   ├── relay.c
│   │   └── CMakeLists.txt
│   │
│   ├── button/                       # Xử lý 5 Nút Bấm
│   │   ├── button.h
│   │   ├── button.c
│   │   └── CMakeLists.txt
│   │
│   ├── input/                        # Xử lý 3 Đầu Vào Khô
│   │   ├── input.h
│   │   ├── input.c
│   │   └── CMakeLists.txt
│   │
│   ├── led_status/                   # LED Báo Trạng Thái
│   │   ├── led_status.h
│   │   ├── led_status.c
│   │   └── CMakeLists.txt
│   │
│   ├── sht/                          # Cảm biến SHT35/SHT40
│   │   ├── sht35.h
│   │   ├── sht35.c
│   │   └── CMakeLists.txt
│   │
│   ├── modbus/                       # Giao thức Modbus RTU
│   │   ├── modbusSRC.h
│   │   ├── modbusSRC.c
│   │   └── CMakeLists.txt
│   │
│   ├── ds3231/                       # RTC DS3231
│   │   ├── ds3231.h
│   │   ├── ds3231.c
│   │   └── CMakeLists.txt
│   │
│   ├── scheduler/                    # Lập Lịch & Tự Động Hóa
│   │   ├── scheduler.h
│   │   ├── scheduler.c
│   │   └── CMakeLists.txt
│   │
│   ├── platform/                     # Cấu Hình Nền Tảng
│   │   ├── platform.h
│   │   ├── platform.c
│   │   └── CMakeLists.txt
│   │
│   └── Ota_update/                   # OTA Update Handler
│       ├── ota.h
│       ├── ota.c
│       └── CMakeLists.txt
│
└── build/                            # Thư mục build (tự sinh)
    ├── firmware.bin
    ├── bootloader.bin
    └── ...
```

---

## 🛠️ Hướng Dẫn Cài Đặt

### 1. Clone Dự Án

```bash
cd ~/Espressif/frameworks
git clone <repo-url> IoT_V1
cd IoT_V1
```

### 2. Cấu Hình Biến Môi Trường

```bash
# Linux/macOS
source ~/esp/esp-idf/export.sh

# Windows (PowerShell)
& "$env:IDF_PATH\install.ps1"
& "$env:IDF_PATH\export.ps1"
```

### 3. Khởi Tạo Submodule (nếu có)

```bash
git submodule update --init --recursive
```

---

## 🔨 Hướng Dẫn Build

### Build Dự Án

```bash
cd ~/Espressif/frameworks/IoT_V1

# Clean build
idf.py fullclean

# Build firmware
idf.py build
```

### Flash Firmware

```bash
# Flash với COM port auto-detect
idf.py -p /dev/ttyUSB0 flash

# Windows
idf.py -p COM3 flash

# Monitor serial output
idf.py -p /dev/ttyUSB0 monitor
```

### Flash Partition Table

```bash
idf.py -p /dev/ttyUSB0 partition-table-flash
```

### OTA Partition Erase (nếu cần)

```bash
esptool.py -p /dev/ttyUSB0 erase_region 0x20000 0x3C0000
```

---

## ⚙️ Cấu Hình Dự Án

### 1. Mở Menu Config

```bash
idf.py menuconfig
```

### 2. Cấu Hình Quan Trọng

**Menuconfig Path**: `Component config → ...`

| Cấu Hình | Giá Trị | Mô Tả |
|---------|--------|-------|
| **WiFi SSID** | `[config]` | WiFi Network Name |
| **WiFi Password** | `[config]` | WiFi Network Password |
| **MQTT Broker** | `wss://iot.croptex.io:443/mqtt` | MQTT Server URI |
| **NTP Server** | `pool.ntp.org` | Network Time Protocol Server |
| **Timezone** | `GMT-7` | Múi giờ (hard-coded trong main.c) |

### 3. Build Configuration Files

**sdkconfig.defaults** (Build-time defaults):
```
CONFIG_ESP32_DEFAULT_CPU_FREQ_240=y
CONFIG_ESP_WIFI_AUTH_WPA2_PSK=y
CONFIG_LWIP_SO_REUSE=y
```

### 4. Flash Cấu Hình Runtime

**main.c** - Các hằng số quan trọng:

```c
#define SHT35_UART      UART_NUM_1
#define UART_TX_PIN     19
#define UART_RX_PIN     18
#define UART_BUF_SIZE   256
```

---

## 📡 Lệnh Điều Khiển

### I. Lệnh MQTT từ App/Server

#### 1. Lệnh Điều Khiển Relay (Command)

**Topic**: `/server/{device_id}/command`

```json
{
  "relay": 1,
  "value": 1
}
```

**Ví dụ:**
```bash
mosquitto_pub -h iot.croptex.io -t "/server/1433bb17-38fd-4faf-ba7d-06d31b4193de/command" \
  -m '{"relay":1,"value":1}'
```

**Response**: `/device/{device_id}/response`
```json
{
  "relay": 1,
  "state": 1,
  "timestamp": 1234567890
}
```

#### 2. Lệnh Cấu Hình (Config)

**Topic**: `/server/{device_id}/config`

**a) Lập Lịch Relay**
```json
{
  "relay1": {
    "dayOfWeek": [1, 2, 3, 4, 5],
    "slots": [
      {"start": "08:00", "stop": "12:00", "active": true},
      {"start": "13:00", "stop": "18:00", "active": true}
    ]
  }
}
```

**b) Cấu Hình Cảm Biến**
```json
{
  "sensorConfig": {
    "type": "temp",
    "active": true,
    "dayOfWeek": [0, 1, 2, 3, 4, 5, 6],
    "start": "00:00",
    "stop": "23:59",
    "tempHigh": 30,
    "tempLow": 15,
    "action1": [1, -1, -1, -1, -1],
    "action2": [0, -1, -1, -1, -1]
  }
}
```

**c) Cấu Hình Input**
```json
{
  "inputConfig": {
    "inputs": [
      {
        "input": 1,
        "actions": [
          {"threshold": 0, "relays": [1, -1, -1, -1, -1]},
          {"threshold": 1, "relays": [0, -1, -1, -1, -1]}
        ]
      }
    ]
  }
}
```

**d) Cấu Hình Danh Sách Cảm Biến**
```json
{
  "sensorNetwork": {
    "count": 2,
    "addresses": [0x01, 0x02]
  }
}
```

#### 3. Lệnh OTA Update

**Topic**: `/server/{device_id}/ota`

```json
{
  "model": "ESP32_v1",
  "url": "https://ota-server.com/firmware-v1.0.2.bin",
  "version": "1.0.2",
  "checksum": "5d41402abc4b2a76b9719d911017c592",
  "job_id": "ota-2024-06-10-001",
  "is_mandatory": false,
  "pending_time_ms": 3600000
}
```

**OTA Status Response**: `/device/{device_id}/ota/status`
```json
{
  "ota": "DOWNLOADING",
  "progress": 45,
  "status": "downloading_firmware",
  "current_version": "1.0.0",
  "new_version": "1.0.2",
  "job_id": "ota-2024-06-10-001"
}
```

### II. Lệnh từ Button (Nút Bấm)

| Button | Hành Động | Kết Quả |
|--------|-----------|---------|
| **BT1** (GPIO 2) | Short Press | Toggle Relay 1 |
| **BT1** (GPIO 2) | Long Press 4s | WiFi BLE Provisioning |
| **BT2** (GPIO 4) | Short Press | Toggle Relay 2 |
| **BT2** (GPIO 4) | Long Press 4s | 4G/PPPoS Connect (Reboot) |
| **BT3** (GPIO 20) | Short Press | Toggle Relay 3 |
| **BT4** (GPIO 23) | Short Press | Toggle Relay 4 |
| **BT5** (GPIO 25) | Short Press | Toggle Relay 5 |
| **BT5** (GPIO 25) | Long Press 4s | Reset WiFi + Restart |

### III. Lệnh từ Dry Contact (Đầu Vào Khô)

Khi trạng thái thay đổi, callback được gọi:

```c
void my_dry_callback_alert(int contact_index, int new_state) {
    scheduler_process_input(contact_index, new_state);
    mqtt_send_status();
}
```

Action được định nghĩa qua MQTT config.

---

## 🏗️ Kiến Trúc Hệ Thống

### Kiến Trúc Layer

```
┌─────────────────────────────────────────────┐
│         Application Layer                   │
│  (Sensor Task, Report Task, Button Handler)│
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│      Component Layer                        │
│  (WiFi, MQTT, Relay, Button, Input, ...)   │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│     ESP-IDF Core (FreeRTOS, Drivers)        │
│  (GPIO, UART, I2C, NVS, Timer, ...)        │
└─────────────────────────────────────────────┘
```

### FreeRTOS Tasks

| Task | Stack (bytes) | Priority | Interval | Chức Năng |
|------|---------------|----------|----------|----------|
| **sensor_task** | 4096 | 5 | 30s | Đọc SHT35 |
| **task_report_status** | 4096 | 5 | 30s | Báo MQTT |
| **button_task** | 2048 | 5 | 10ms polling | Xử lý button |
| **dry_contact_monitor_task** | 2048 | 5 | 10ms polling | Xử lý input |
| **MQTT Event Loop** | > 4096 | 5 | Event-driven | MQTT Pub/Sub |

### Mutex/Semaphore Protection

| Tài Nguyên | Loại | Chức Năng |
|-----------|------|----------|
| **sensor_data_mutex** | Mutex | Bảo vệ `current_temp`, `current_hum`, `sensor_valid` |
| **scheduler_mutex** | Mutex | Bảo vệ schedule config & sensor config |
| **rtc_mutex** | Mutex | Bảo vệ RTC access |

---

## 📍 Pinout & GPIO Mapping

### Relay Pins

```
RL1 → GPIO 13
RL2 → GPIO 12
RL3 → GPIO 14
RL4 → GPIO 27
RL5 → GPIO 26
```

### Button Pins

```
BT1 → GPIO 2  (WiFi Setup)
BT2 → GPIO 4  (4G Connect)
BT3 → GPIO 20 (Custom)
BT4 → GPIO 23 (Custom)
BT5 → GPIO 25 (WiFi Reset)
```

### Dry Contact Pins

```
CONTACT_1 → GPIO 32
CONTACT_2 → GPIO 34
CONTACT_3 → GPIO 35
```

### LED Status

```
LED_STATUS → GPIO 33 (Active Low: 0=ON, 1=OFF)
```

### Sensor UART

```
SHT35 Modbus:
  TX → GPIO 19 (UART_NUM_1)
  RX → GPIO 18 (UART_NUM_1)
  Baudrate: 9600
```

### Modem PPPoS UART

```
Modem Serial:
  TX → GPIO 17 (UART_NUM_2)
  RX → GPIO 16 (UART_NUM_2)
  Reset → GPIO 5
```

### DS3231 RTC I2C

```
SDA → GPIO 21 (I2C_NUM_0)
SCL → GPIO 22 (I2C_NUM_0)
Address: 0x68
```

---

## 💾 NVS Storage

### Namespace: "storage"

| Key | Type | Mô Tả |
|-----|------|-------|
| `RUN_MODEM` | i32 | App Mode (0=idle, 1=WiFi config, 2=4G) |
| `json_sched` | blob | RelaySchedule_t[5] - Lịch relay |
| `sensor_blob` | blob | sensor_config_t - Cấu hình cảm biến |
| `sensor_network` | blob | sensor_network_t - Danh sách cảm biến |

### Namespace: "relay_storage"

| Key | Type | Mô Tả |
|-----|------|-------|
| `relay_1` | i32 | Trạng thái Relay 1 |
| `relay_2` | i32 | Trạng thái Relay 2 |
| `relay_3` | i32 | Trạng thái Relay 3 |
| `relay_4` | i32 | Trạng thái Relay 4 |
| `relay_5` | i32 | Trạng thái Relay 5 |

### Namespace: "platform"

| Key | Type | Mô Tả |
|-----|------|-------|
| `last_time` | i32 | Sao lưu thời gian hệ thống |
| `last_ntp_sync` | i32 | Lần NTP sync cuối cùng |
| `ntp_fails` | u32 | Số lần NTP sync thất bại |

---

## 📡 MQTT Topics

### Status Topics (Device → Server)

```
/device/{device_id}/status
/device/{device_id}/response
/device/{device_id}/sensor/{addr}
/device/{device_id}/ota/status
/device/{device_id}/info
```

### Command Topics (Server → Device)

```
/server/{device_id}/command
/server/{device_id}/config
/server/{device_id}/ota
```

### Last Will Message

**Topic**: `/device/{device_id}/status`

```json
{
  "device_id": "1433bb17-...",
  "status": 0
}
```

---

## 📊 Status Response Format

### Device Status

```json
{
  "device_id": "1433bb17-38fd-4faf-ba7d-06d31b4193de",
  "status": 1,
  "relays": [0, 1, 0, 1, 0],
  "inputs": [1, 0, 1],
  "temp": 25.5,
  "hum": 45.2,
  "timestamp": 1718000000
}
```

### Device Info

```json
{
  "model": "ESP32_v1",
  "relay": 5,
  "firmware_version": "1.0.0"
}
```

### Sensor Data

```json
{
  "addr": 1,
  "temp": 25.5,
  "hum": 45.2
}
```

---

## 🔍 Troubleshooting

### Vấn Đề: Không Kết Nối WiFi

**Nguyên nhân**: SSID/Password sai hoặc WiFi không có sẵn

**Giải pháp**:
```bash
# Nhấn giữ BT1 (4s) để vào BLE Provisioning
# Hoặc sử dụng serial terminal:
idf.py -p /dev/ttyUSB0 monitor
```

### Vấn Đề: MQTT Không Kết Nối

**Nguyên nhân**: Broker URL sai hoặc firewall chặn

**Giải pháp**:
1. Kiểm tra URL MQTT: `wss://iot.croptex.io:443/mqtt`
2. Kiểm tra certificate bundle
3. Xem log: `idf.py -p /dev/ttyUSB0 monitor | grep MQTT`

### Vấn Đề: Cảm Biến Không Đọc Được

**Nguyên nhân**: UART config sai hoặc cảm biến disconnect

**Giải pháp**:
```bash
# Kiểm tra log
idf.py -p /dev/ttyUSB0 monitor | grep "SHT35"

# Kiểm tra kết nối UART:
# TX: GPIO 19, RX: GPIO 18, GND
# Baudrate: 9600
```

### Vấn Đề: Relay Không Hoạt Động

**Nguyên nhân**: GPIO level logic sai hoặc relay module sai

**Giải pháp**:
1. Kiểm tra GPIO output level
2. Kiểm tra relay module (active high/low)
3. Kiểm tra power supply (5V/12V)

### Vấn Đề: OTA Update Thất Bại

**Nguyên nhân**: URL firmware sai, checksum không match, hoặc download timeout

**Giải pháp**:
```bash
# Kiểm tra log OTA
idf.py -p /dev/ttyUSB0 monitor | grep OTA

# Verify checksum MD5
md5sum firmware.bin

# Xem trạng thái OTA
mosquitto_sub -h iot.croptex.io -t "/device/*/ota/status"
```

### Vấn Đề: NVS Flash Bị Corrupt

**Nguyên nhân**: Flash bị ghi đè hoặc power loss không kỳ vọng

**Giải pháp**:
```bash
# Erase NVS partition
idf.py -p /dev/ttyUSB0 erase-region 0x9000 0x6000

# Rebuild & Flash
idf.py -p /dev/ttyUSB0 build flash
```

### Vấn Đề: Heap Memory Không Đủ

**Nguyên nhân**: Memory leak hoặc buffer size quá lớn

**Giải pháp**:
```bash
# Kiểm tra Heap free
idf.py -p /dev/ttyUSB0 monitor | grep "Heap:"

# Giảm buffer size hoặc stack size
# Kiểm tra file `*_config.h` của các component
```

---

## 📝 Logs & Debugging

### Enable Debug Logging

**menuconfig**:
```
Component config → Log output → Default log verbosity → Debug
```

### Serial Monitor

```bash
# Bắt đầu monitor
idf.py -p /dev/ttyUSB0 monitor

# Với baud rate tùy chỉnh
idf.py -p /dev/ttyUSB0 -b 115200 monitor

# Grep specific module
idf.py -p /dev/ttyUSB0 monitor | grep "MQTT"
```

### TAG dùng trong project

- `MAIN` - Main application
- `WIFI_PROV` - WiFi Provisioning
- `MQTT_COMPONENT` - MQTT
- `RELAY` - Relay control
- `BUTTON` - Button handling
- `DRY_CONTACT` - Dry input
- `LED_STATUS` - LED indicator
- `SHT35` - Sensor
- `SCHEDULER` - Scheduler
- `PPPOS_COMPONENT` - PPPoS modem
- `OTA` - OTA update
- `PLATFORM` - Platform utilities

---

## 🚀 Production Checklist

- [ ] WiFi credentials được cấu hình
- [ ] MQTT broker URL được xác minh
- [ ] OTA URL được setup
- [ ] DS3231 RTC được khôi tạo
- [ ] Relay GPIO được kiểm tra
- [ ] Button debounce được calibrate
- [ ] Sensor Modbus address được config
- [ ] NVS partition đủ không gian
- [ ] Firmware version được set đúng
- [ ] Log level được set production (Warn+)
- [ ] Power supply ổn định (5V, 3.3V)
- [ ] Certificate bundle được include
- [ ] Partition table được flash đúng

---

## 📚 Tài Liệu Tham Khảo

- **ESP-IDF Docs**: https://docs.espressif.com/projects/esp-idf/
- **MQTT Protocol**: https://mqtt.org/
- **Modbus RTU**: http://www.modbus.org/
- **SHT35 Datasheet**: https://sensirion.com/products/catalog/SHT35-DIS-B/
- **DS3231 RTC**: https://datasheets.maximintegrated.com/en/ds/DS3231.pdf

---

## 📞 Support & Contribution

Để báo cáo lỗi hoặc đóng góp:

1. **Issue Tracker**: Create an issue với `[BUG]` hoặc `[FEATURE]` tag
2. **Pull Request**: Gửi PR với mô tả chi tiết
3. **Email**: support@company.com

---

## 📄 License

Dự án này sử dụng **Apache License 2.0**

```
Copyright 2024 - IoT_V1 Team

Licensed under the Apache License, Version 2.0
```

---

## ✅ Version History

| Version | Date | Notes |
|---------|------|-------|
| 1.0.0 | 2024-06-10 | Initial Release |
| 1.0.1 | TBD | Bug fixes |
| 1.1.0 | TBD | New features |

---

**Last Updated**: 2024-06-10  
**Maintainer**: IoT_V1 Development Team  
**Status**: 🟢 Production Ready
