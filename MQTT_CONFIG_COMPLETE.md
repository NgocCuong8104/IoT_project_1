# 📡 Tổng Hợp Đầy Đủ JSON MQTT Config - IoT_V1

**Phiên bản**: 1.0.0  
**Ngày cập nhật**: 2024-06-10  
**Device Model**: ESP32_v1

---

## 📋 Mục Lục

1. [MQTT Topics](#mqtt-topics)
2. [Command Topic - Điều Khiển Relay](#command-topic---điều-khiển-relay)
3. [Config Topic - Cấu Hình Thiết Bị](#config-topic---cấu-hình-thiết-bị)
4. [OTA Topic - Cập Nhật Firmware](#ota-topic---cập-nhật-firmware)
5. [Status Topic - Trạng Thái Thiết Bị](#status-topic---trạng-thái-thiết-bị)
6. [Publish Topics](#publish-topics)

---

## 📍 MQTT Topics

### Subscribe Topics (Thiết bị lắng nghe)

| Topic | Mục Đích |
|-------|---------|
| `/server/{device_id}/command` | Lệnh điều khiển relay |
| `/server/{device_id}/config` | Cấu hình thiết bị (relay, sensor, input, OTA, etc.) |
| `/server/{device_id}/ota` | OTA firmware update trigger |

### Publish Topics (Thiết bị gửi dữ liệu)

| Topic | Nội Dung |
|-------|---------|
| `/device/{device_id}/status` | Trạng thái thiết bị (relay, input, sensor, timestamp) |
| `/device/{device_id}/response` | Phản hồi lệnh relay |
| `/device/{device_id}/sensor/{addr}` | Dữ liệu từng cảm biến (temp, humidity) |
| `/device/{device_id}/ota/status` | Trạng thái OTA update |
| `/device/{device_id}/info` | Thông tin thiết bị (model, firmware version) |

---

## 🎮 Command Topic - Điều Khiển Relay

### Topic: `/server/{device_id}/command`

**Chức năng**: Điều khiển ON/OFF các relay

### JSON Format:

```json
{
  "relay": 1,
  "value": 1
}
```

### Chi Tiết:

| Field | Type | Range | Ý Nghĩa |
|-------|------|-------|---------|
| `relay` | int | 1-5 | Số relay (1=RL1, 2=RL2, ..., 5=RL5) |
| `value` | int | 0/1 | 0=OFF, 1=ON |

### Ví Dụ:

```bash
# Bật Relay 1
mosquitto_pub -h iot.croptex.io \
  -t "/server/1433bb17-38fd-4faf-ba7d-06d31b4193de/command" \
  -m '{"relay": 1, "value": 1}'

# Tắt Relay 3
mosquitto_pub -h iot.croptex.io \
  -t "/server/1433bb17-38fd-4faf-ba7d-06d31b4193de/command" \
  -m '{"relay": 3, "value": 0}'
```

### Phản Hồi:

Thiết bị sẽ publish:
- Topic: `/device/{device_id}/response`
- Payload:
```json
{
  "relay": 1,
  "state": 1,
  "timestamp": 1718000000
}
```

---

## ⚙️ Config Topic - Cấu Hình Thiết Bị

### Topic: `/server/{device_id}/config`

**Chức năng**: Cấu hình toàn bộ thiết bị

---

### 1️⃣ Cấu Hình Relay Lịch Trình

#### Format:

```json
{
  "relay1": {
    "dayOfWeek": [0, 1, 2, 3, 4, 5, 6],
    "slots": [
      {
        "timeStart": {"h": 8, "m": 0},
        "timeStop": {"h": 17, "m": 0},
        "active": true
      }
    ]
  },
  "relay2": {
    "dayOfWeek": [1, 2, 3, 4, 5],
    "slots": [
      {
        "timeStart": {"h": 6, "m": 30},
        "timeStop": {"h": 22, "m": 0},
        "active": true
      }
    ]
  }
}
```

#### Chi Tiết:

| Field | Type | Ý Nghĩa |
|-------|------|---------|
| `relay1-5` | object | Cấu hình cho relay 1-5 |
| `dayOfWeek` | int[] | Ngày hoạt động (0=CN, 1=T2, ..., 6=T7) |
| `slots[].timeStart` | object | Giờ bật relay {h: 0-23, m: 0-59} |
| `slots[].timeStop` | object | Giờ tắt relay {h: 0-23, m: 0-59} |
| `slots[].active` | bool | Kích hoạt slot này |

#### Ví Dụ - Bật RL1 hàng ngày 8:00-17:00:

```json
{
  "relay1": {
    "dayOfWeek": [0, 1, 2, 3, 4, 5, 6],
    "slots": [
      {
        "timeStart": {"h": 8, "m": 0},
        "timeStop": {"h": 17, "m": 0},
        "active": true
      }
    ]
  }
}
```

#### Ví Dụ - Bật RL2 Thứ 2-6, 6:30-22:00:

```json
{
  "relay2": {
    "dayOfWeek": [1, 2, 3, 4, 5],
    "slots": [
      {
        "timeStart": {"h": 6, "m": 30},
        "timeStop": {"h": 22, "m": 0},
        "active": true
      }
    ]
  }
}
```

#### Ví Dụ - Bật RL3 qua đêm 22:00 (T6) → 6:00 (T2):

```json
{
  "relay3": {
    "dayOfWeek": [1, 2, 3, 4, 5],
    "slots": [
      {
        "timeStart": {"h": 22, "m": 0},
        "timeStop": {"h": 6, "m": 0},
        "active": true
      }
    ]
  }
}
```

---

### 2️⃣ Cấu Hình Cảm Biến (Sensor Config)

#### Format - Logic Nhiệt Độ:

```json
{
  "sensor": {
    "type": "temp",
    "active": true,
    "dayOfWeek": [0, 1, 2, 3, 4, 5, 6],
    "timeStart": {"h": 0, "m": 0},
    "timeStop": {"h": 23, "m": 59},
    "temp_high": 30.0,
    "temp_low": 15.0,
    "hysteresis_temp": 0.5,
    "action1": [1, 0, -1, -1, -1],
    "action2": [0, 1, -1, -1, -1]
  }
}
```

#### Format - Logic Độ Ẩm:

```json
{
  "sensor": {
    "type": "humidity",
    "active": true,
    "dayOfWeek": [0, 1, 2, 3, 4, 5, 6],
    "timeStart": {"h": 0, "m": 0},
    "timeStop": {"h": 23, "m": 59},
    "humi_high": 80.0,
    "humi_low": 30.0,
    "hysteresis_humi": 2.0,
    "action1": [0, 1, -1, -1, -1],
    "action2": [0, 0, -1, -1, -1]
  }
}
```

#### Chi Tiết:

| Field | Type | Ý Nghĩa |
|-------|------|---------|
| `type` | string | "temp" hoặc "humidity" |
| `active` | bool | Bật/tắt logic này |
| `dayOfWeek` | int[] | Ngày hoạt động |
| `timeStart` | object | Giờ bắt đầu {h, m} |
| `timeStop` | object | Giờ kết thúc {h, m} |
| `temp_high` | float | Ngưỡng nhiệt độ cao (°C) |
| `temp_low` | float | Ngưỡng nhiệt độ thấp (°C) |
| `humi_high` | float | Ngưỡng độ ẩm cao (%) |
| `humi_low` | float | Ngưỡng độ ẩm thấp (%) |
| `hysteresis_temp` | float | Hysteresis nhiệt độ (°C) |
| `hysteresis_humi` | float | Hysteresis độ ẩm (%) |
| `action1` | int[5] | Relay action khi vượt ngưỡng cao (1=ON, 0=OFF, -1=skip) |
| `action2` | int[5] | Relay action khi dưới ngưỡng thấp |

#### Ví Dụ - Bật RL1 khi temp > 30°C, tắt khi temp < 15°C:

```json
{
  "sensor": {
    "type": "temp",
    "active": true,
    "dayOfWeek": [0, 1, 2, 3, 4, 5, 6],
    "timeStart": {"h": 0, "m": 0},
    "timeStop": {"h": 23, "m": 59},
    "temp_high": 30.0,
    "temp_low": 15.0,
    "hysteresis_temp": 0.5,
    "action1": [1, -1, -1, -1, -1],
    "action2": [0, -1, -1, -1, -1]
  }
}
```

#### Ví Dụ - Bật RL1+RL2 khi humidity > 80%, tắt cả 2 khi < 30%:

```json
{
  "sensor": {
    "type": "humidity",
    "active": true,
    "dayOfWeek": [0, 1, 2, 3, 4, 5, 6],
    "timeStart": {"h": 0, "m": 0},
    "timeStop": {"h": 23, "m": 59},
    "humi_high": 80.0,
    "humi_low": 30.0,
    "hysteresis_humi": 2.0,
    "action1": [1, 1, -1, -1, -1],
    "action2": [0, 0, -1, -1, -1]
  }
}
```

---

### 3️⃣ Cấu Hình Input (Dry Contact)

#### Format:

```json
{
  "input1": {
    "active": true,
    "dayOfWeek": [0, 1, 2, 3, 4, 5, 6],
    "slots": [
      {
        "timeStart": {"h": 0, "m": 0},
        "timeStop": {"h": 23, "m": 59},
        "active": true
      }
    ],
    "constraint": [-1, -1, -1, -1, -1],
    "actions": [
      {
        "threshold_value": 1,
        "relays": [1, 0, -1, -1, -1]
      }
    ]
  }
}
```

#### Chi Tiết:

| Field | Type | Ý Nghĩa |
|-------|------|---------|
| `input1-3` | object | Cấu hình cho input 1-3 |
| `active` | bool | Bật/tắt input |
| `dayOfWeek` | int[] | Ngày hoạt động |
| `slots[].timeStart` | object | Giờ bắt đầu {h, m} |
| `slots[].timeStop` | object | Giờ kết thúc {h, m} |
| `constraint[1-5]` | int | Trạng thái relay cần thiết (-1=any, 0=OFF, 1=ON) |
| `actions[].threshold_value` | int | Giá trị trigger (0=OPEN, 1=CLOSED) |
| `actions[].relays[1-5]` | int | Relay action (1=ON, 0=OFF, -1=skip) |

#### Ví Dụ - Input 1 Đóng → Bật RL1:

```json
{
  "input1": {
    "active": true,
    "dayOfWeek": [0, 1, 2, 3, 4, 5, 6],
    "slots": [
      {
        "timeStart": {"h": 0, "m": 0},
        "timeStop": {"h": 23, "m": 59},
        "active": true
      }
    ],
    "constraint": [-1, -1, -1, -1, -1],
    "actions": [
      {
        "threshold_value": 1,
        "relays": [1, -1, -1, -1, -1]
      }
    ]
  }
}
```

#### Ví Dụ - Input 2 Mở (0) → Tắt RL2, chỉ khi RL1 đang ON:

```json
{
  "input2": {
    "active": true,
    "dayOfWeek": [0, 1, 2, 3, 4, 5, 6],
    "slots": [
      {
        "timeStart": {"h": 0, "m": 0},
        "timeStop": {"h": 23, "m": 59},
        "active": true
      }
    ],
    "constraint": [1, -1, -1, -1, -1],
    "actions": [
      {
        "threshold_value": 0,
        "relays": [-1, 0, -1, -1, -1]
      }
    ]
  }
}
```

---

### 4️⃣ Cấu Hình Danh Sách Cảm Biến (Sensor List)

#### Format:

```json
{
  "sensor_list": {
    "addresses": [1, 2, 3]
  }
}
```

#### Chi Tiết:

| Field | Type | Range | Ý Nghĩa |
|-------|------|-------|---------|
| `addresses` | int[] | 1-247 | Danh sách địa chỉ Modbus cảm biến |

#### Ví Dụ - 3 cảm biến:

```json
{
  "sensor_list": {
    "addresses": [1, 2, 3]
  }
}
```

#### Ví Dụ - 1 cảm biến (reset):

```json
{
  "sensor_list": {
    "addresses": [1]
  }
}
```

---

### 5️⃣ Cấu Hình Auto-ID (Đổi Địa Chỉ Cảm Biến Tự Động)

#### Format:

```json
{
  "auto_id": {
    "new_id": 2
  }
}
```

#### Chi Tiết:

| Field | Type | Range | Ý Nghĩa |
|-------|------|-------|---------|
| `new_id` | int | 1-247 | Địa chỉ Modbus mới |

#### Ví Dụ - Đổi cảm biến từ ID 0 → ID 2:

```json
{
  "auto_id": {
    "new_id": 2
  }
}
```

---

### 🔧 Config Toàn Bộ (Combined JSON)

#### Format:

```json
{
  "relay1": {
    "dayOfWeek": [0, 1, 2, 3, 4, 5, 6],
    "slots": [
      {
        "timeStart": {"h": 8, "m": 0},
        "timeStop": {"h": 17, "m": 0},
        "active": true
      }
    ]
  },
  "relay2": {
    "dayOfWeek": [1, 2, 3, 4, 5],
    "slots": [
      {
        "timeStart": {"h": 6, "m": 30},
        "timeStop": {"h": 22, "m": 0},
        "active": true
      }
    ]
  },
  "sensor": {
    "type": "temp",
    "active": true,
    "dayOfWeek": [0, 1, 2, 3, 4, 5, 6],
    "timeStart": {"h": 0, "m": 0},
    "timeStop": {"h": 23, "m": 59},
    "temp_high": 30.0,
    "temp_low": 15.0,
    "hysteresis_temp": 0.5,
    "action1": [1, 0, -1, -1, -1],
    "action2": [0, 1, -1, -1, -1]
  },
  "input1": {
    "active": true,
    "dayOfWeek": [0, 1, 2, 3, 4, 5, 6],
    "slots": [
      {
        "timeStart": {"h": 0, "m": 0},
        "timeStop": {"h": 23, "m": 59},
        "active": true
      }
    ],
    "constraint": [-1, -1, -1, -1, -1],
    "actions": [
      {
        "threshold_value": 1,
        "relays": [1, -1, -1, -1, -1]
      }
    ]
  },
  "sensor_list": {
    "addresses": [1, 2, 3]
  }
}
```

---

## 🔄 OTA Topic - Cập Nhật Firmware

### Topic: `/server/{device_id}/ota`

#### Format:

```json
{
  "url": "https://example.com/firmware.bin",
  "version": "1.0.1",
  "checksum": "abc123def456",
  "job_id": "ota_job_001",
  "model": "ESP32_v1"
}
```

#### Chi Tiết:

| Field | Type | Ý Nghĩa |
|-------|------|---------|
| `url` | string | HTTPS URL tới firmware bin file |
| `version` | string | Phiên bản firmware (so sánh với hiện tại) |
| `checksum` | string | MD5 checksum của firmware |
| `job_id` | string | ID công việc OTA (cho tracking) |
| `model` | string | Model thiết bị (ESP32_v1) |

#### Ví Dụ:

```bash
mosquitto_pub -h iot.croptex.io \
  -t "/server/1433bb17-38fd-4faf-ba7d-06d31b4193de/ota" \
  -m '{
    "url": "https://s3.amazonaws.com/firmware-v1.0.1.bin",
    "version": "1.0.1",
    "checksum": "5d41402abc4b2a76b9719d911017c592",
    "job_id": "ota_20240610_001",
    "model": "ESP32_v1"
  }'
```

#### Trạng Thái OTA:

Thiết bị publish trạng thái tới:
- Topic: `/device/{device_id}/ota/status`
- Payload:
```json
{
  "job_id": "ota_20240610_001",
  "status": "downloading",
  "progress": 45,
  "version": "1.0.1"
}
```

**Status values**: `downloading`, `validating`, `flashing`, `success`, `failed`

---

## 📊 Status Topic - Trạng Thái Thiết Bị

### Topic: `/device/{device_id}/status`

**Publish interval**: 30 giây (tự động)

#### Format:

```json
{
  "device_id": "1433bb17-38fd-4faf-ba7d-06d31b4193de",
  "status": 1,
  "relays": [1, 0, 0, 1, 0],
  "inputs": [1, 0, 1],
  "temp": 27.3,
  "hum": 55.2,
  "timestamp": 1718000000
}
```

#### Chi Tiết:

| Field | Type | Ý Nghĩa |
|-------|------|---------|
| `device_id` | string | UUID thiết bị |
| `status` | int | 1=online, 0=offline |
| `relays` | int[5] | Trạng thái RL1-RL5 (1=ON, 0=OFF) |
| `inputs` | int[3] | Trạng thái INPUT1-3 (1=CLOSED, 0=OPEN) |
| `temp` | float | Nhiệt độ hiện tại (°C) |
| `hum` | float | Độ ẩm hiện tại (%) |
| `timestamp` | int | Unix timestamp |

---

## 📤 Publish Topics

### 1. Sensor Data Detail

**Topic**: `/device/{device_id}/sensor/{addr}`

Mỗi cảm biến trong danh sách publish riêng:

```json
{
  "addr": 1,
  "temp": 27.3,
  "hum": 55.2
}
```

**Ví dụ**:
```
/device/1433bb17-38fd-4faf-ba7d-06d31b4193de/sensor/1
/device/1433bb17-38fd-4faf-ba7d-06d31b4193de/sensor/2
/device/1433bb17-38fd-4faf-ba7d-06d31b4193de/sensor/3
```

### 2. Device Info

**Topic**: `/device/{device_id}/info`

Publish lần đầu khi MQTT kết nối:

```json
{
  "model": "ESP32_v1",
  "relay": 5,
  "firmware_version": "1.0.0"
}
```

### 3. Response (Relay Command Response)

**Topic**: `/device/{device_id}/response`

Sau mỗi lệnh command:

```json
{
  "relay": 1,
  "state": 1,
  "timestamp": 1718000000
}
```

---

## 🚀 Cách Sử Dụng

### 1. Gửi Command (Điều khiển relay):

```bash
mosquitto_pub -h iot.croptex.io \
  -t "/server/device-id/command" \
  -m '{"relay": 1, "value": 1}'
```

### 2. Gửi Config (Lịch trình relay):

```bash
mosquitto_pub -h iot.croptex.io \
  -t "/server/device-id/config" \
  -m '{
    "relay1": {
      "dayOfWeek": [0,1,2,3,4,5,6],
      "slots": [{
        "timeStart": {"h": 8, "m": 0},
        "timeStop": {"h": 17, "m": 0},
        "active": true
      }]
    }
  }'
```

### 3. Auto-ID (Đổi địa chỉ cảm biến):

```bash
mosquitto_pub -h iot.croptex.io \
  -t "/server/device-id/config" \
  -m '{"auto_id": {"new_id": 2}}'
```

### 4. Subscribe Status (Lắng nghe trạng thái):

```bash
mosquitto_sub -h iot.croptex.io \
  -t "/device/device-id/status"
```

---

## ⚠️ Lỗi Thường Gặp

### 1. JSON Malformed

```
E (12345) SCHEDULER: JSON Malformed!
```

**Giải pháp**: Kiểm tra format JSON (quotes, braces, etc.)

### 2. Value Out of Range

```
W (12345) SCHEDULER: Missing or invalid integer key: relay
```

**Giải pháp**: Kiểm tra type dữ liệu (int vs string)

### 3. Model Mismatch (OTA)

```
W (12345) MQTT_COMPONENT: Model mismatch - expected: ESP32_v1, got: ESP32_v2
```

**Giải pháp**: Kiểm tra field `model` trong OTA message

### 4. Config Không Cập Nhật

**Nguyên nhân**: 
- MQTT chưa kết nối
- Device ID sai
- Topic sai

**Kiểm tra**:
```bash
# Xem log thiết bị
idf.py -p /dev/ttyUSB0 monitor | grep "config"

# Kiểm tra MQTT connection
idf.py -p /dev/ttyUSB0 monitor | grep "MQTT_EVENT"
```

---

## 📝 Bảng Tham Chiếu Nhanh

### Device ID

```
1433bb17-38fd-4faf-ba7d-06d31b4193de (UUID cố định)
```

### Relay Index

```
1 = GPIO13 (RL1)
2 = GPIO12 (RL2)
3 = GPIO14 (RL3)
4 = GPIO27 (RL4)
5 = GPIO26 (RL5)
```

### Input Index

```
1 = GPIO32 (CONTACT_1)
2 = GPIO34 (CONTACT_2)
3 = GPIO35 (CONTACT_3)
```

### Ngày trong Tuần

```
0 = Chủ nhật (Sunday)
1 = Thứ 2 (Monday)
2 = Thứ 3 (Tuesday)
3 = Thứ 4 (Wednesday)
4 = Thứ 5 (Thursday)
5 = Thứ 6 (Friday)
6 = Thứ 7 (Saturday)
```

### Cảm Biến

```
Type: "temp" hoặc "humidity"
Modbus Address: 1-247
Max: 10 cảm biến
```

---

## 📚 Kết Nối Thông Số

| Tham Số | Giá Trị |
|---------|--------|
| Broker URL | wss://iot.croptex.io:443/mqtt |
| Protocol | MQTT v3.1.1 |
| Keep-alive | 60 giây |
| QoS (Command) | 0 |
| QoS (Config) | 0 |
| QoS (Status) | 1 (persistent) |
| LWT Topic | `/device/{device_id}/status` |
| LWT Message | `{"device_id":"...","status":0}` |

---

## 💾 NVS Storage

| Namespace | Key | Size | Nội Dung |
|-----------|-----|------|---------|
| storage | json_sched | 2000 bytes | Relay schedules |
| storage | sensor_blob | 500 bytes | Sensor config |
| storage | snsr_net | 11 bytes | Sensor network list |
| storage | input_state | 200 bytes | Input config |
| relay_storage | relay_1-5 | 1 byte each | Relay state (persistent) |

---

## 🔒 Security Notes

1. **WSS (WebSocket Secure)**: Tất cả traffic encrypted
2. **Device ID**: UUID cố định trong code (không thay đổi qua config)
3. **LWT**: Last Will Message để detect offline device
4. **NVS**: Selective write (chỉ ghi nếu có thay đổi) để tránh wear

---

**Tài Liệu này được tạo từ**: scheduler.c + mqtt.c  
**Ngày cập nhật**: 2024-06-10  
**Phiên bản**: IoT_V1 v1.0.0
