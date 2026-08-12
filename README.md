# IoT_V1

| Các Nền Tảng Được Hỗ Trợ | ESP32 | ESP32-C2 | ESP32-C3 | ESP32-C5 | ESP32-C6 | ESP32-H2 | ESP32-P4 | ESP32-S2 | ESP32-S3 |
| ----------------------- | ----- | -------- | -------- | -------- | -------- | -------- | -------- | -------- | -------- |

## 📋 Tổng Quan Dự Án

IoT_V1 là một giải pháp IoT mô-đun, sẵn sàng sản xuất được xây dựng trên ESP-IDF. Nó cung cấp một kiến trúc linh hoạt để kết nối các thiết bị IoT với các nền tảng đám mây thông qua MQTT đồng thời hỗ trợ thu thập và kiểm soát dữ liệu cục bộ.

## 🏗️ Kiến Trúc Dự Án

### Các Tính Năng Chính

- **Kết Nối Kép**: Hỗ trợ WiFi + PPPoS
- **Tích Hợp Đám Mây**: Hỗ trợ giao thức MQTT cho truyền dữ liệu thời gian thực
- **Hỗ Trợ Cảm Biến**: Cảm biến nhiệt độ/độ ẩm SHT35/SHT40 qua Modbus
- **Điều Khiển Thiết Bị**: Quản lý relay và đèn báo trạng thái
- **Thời Gian Thực**: DS3231 RTC để ghi lại dấu thời gian chính xác
- **Cập Nhật OTA**: Khả năng cập nhật firmware trên không khí
- **Lập Lịch Nhiệm Vụ**: Lập lịch nhiệm vụ nâng cao cho các hoạt động định kỳ
- **Nhập Liệu Người Dùng**: Xử lý nhập từ nút bấm với chống rung
- **Tính Toàn Vẹn Dữ Liệu**: Truy cập dữ liệu cảm biến được bảo vệ bằng Mutex để an toàn luồng

### Cấu Trúc Thư Mục

```
IoT_V1/
├── CMakeLists.txt              # Cấu hình xây dựng dự án
├── main/                         # Điểm nhập của ứng dụng chính
│   ├── CMakeLists.txt
│   └── main.c                   # Khởi tạo ứng dụng & quản lý nhiệm vụ
├── components/                   # Thư viện thành phần mô-đun
│   ├── wifi/                    # Mô-đun kết nối WiFi
│   ├── app_mqtt/                # Quản lý máy khách MQTT & pub/sub
│   ├── pppos/                   # Kết nối serial PPPoS (bản sao lưu)
│   ├── sht/                     # Trình điều khiển cảm biến SHT35/SHT40
│   ├── modbus/                  # Triển khai giao thức Modbus RTU
│   ├── ds3231/                  # Trình điều khiển mô-đun RTC DS3231
│   ├── relay/                   # Mô-đun điều khiển relay
│   ├── led_status/              # Quản lý chỉ báo trạng thái LED
│   ├── button/                  # Trình xử lý nhập từ nút bấm
│   ├── input/                   # Xử lý nhập liệu chung
│   ├── scheduler/               # Công cụ lập lịch nhiệm vụ
│   ├── platform/                # Cấu hình cụ thể nền tảng
│   └── ota_update/              # Trình xử lý cập nhật trên không khí
├── partitions.csv              # Sơ đồ phân chia bộ nhớ flash
├── sdkconfig                   # Cấu hình thời gian xây dựng
├── sdkconfig.defaults          # Giá trị cấu hình mặc định
```

### Kiến Trúc Thành Phần

#### **Các Thành Phần Kết Nối Cốt Lõi**

- **Thành Phần WiFi**: Xử lý kết nối WiFi, kết nối lại và quản lý sự kiện
- **Thành Phần PPPoS**: Cung cấp kết nối dự phòng qua mạng di động/serial
- **Thành Phần MQTT**: Quản lý vòng đời máy khách MQTT, hoạt động pub/sub và xếp hàng tin nhắn

#### **Các Thành Phần Cảm Biến & Nhập/Xuất Thiết Bị**

- **Thành Phần SHT**: Giao diện I2C/Modbus cho cảm biến nhiệt độ & độ ẩm
- **Thành Phần Modbus**: Giao thức RTU để thu thập dữ liệu cảm biến và truy cập thanh ghi
- **Thành Phần Relay**: Điều khiển GPIO để kích hoạt/vô hiệu hóa relay
- **Thành Phần Đèn Báo Trạng Thái**: Phản hồi trực quan cho trạng thái hệ thống
- **Thành Phần Nút Bấm**: Nhập liệu người dùng với bảo vệ chống rung
- **Thành Phần DS3231**: Đồng bộ hóa và lấy dữ liệu RTC

#### **Dịch Vụ Hệ Thống**

- **Thành Phần Lập Lịch**: Thực thi nhiệm vụ dựa trên thời gian và lập lịch sự kiện
- **Thành Phần Cập Nhật OTA**: Quản lý cập nhật firmware an toàn
- **Thành Phần Nền Tảng**: Lớp trừu tượng phần cứng cho GPIO, UART và các ngoại vi

### Kiến Trúc Luồng Dữ Liệu

```text
┌─────────────────────────────────────────────────────────────┐
│              Ứng Dụng Chính                                 │
│                (main.c)                                     │
└─────────────────┬───────────────────────────────────────────┘
                  │
    ┌─────────────┼─────────────┬──────────────────┐
    │             │             │                  │
┌───▼─────┐  ┌──▼──────┐  ┌────▼─────┐      ┌─────▼──┐
│ Cảm Biến│  │ Thiết Bị│  │ Điều Khiển│      │ Cấu Hình│
├─────────┤  ├─────────┤  ├───────────┤      ├────────┤
│ SHT35   │  │ Relay   │  │ Nút Bấm   │      │ MQTT   │
│ DS3231  │  │ LED     │  │ Lập Lịch  │      │ WiFi   │
└────┬────┘  └────┬────┘  └─────┬─────┘      │ PPPoS  │
     │            │             │            │ Modbus │
     └─────┬──────┴─────────────┴────────────┘        │
           │     Lớp Kết Nối                          │
       ┌───▼──────────────────────────────────────────┘
       │
   ┌───▼──────────────────────┐
   │   Hệ Thống Đám Mây/Từ Xa  │
   │  (qua WiFi/PPPoS/MQTT)   │
   └──────────────────────────┘
```

## 🔧 Xây Dựng & Biên Dịch

Dự án này sử dụng hệ thống xây dựng CMake và ESP-IDF:

```bash
# Cấu hình dự án
idf.py menuconfig

# Xây dựng dự án
idf.py build

# Nạp vào thiết bị
idf.py -p COM_PORT flash

# Giám sát đầu ra serial
idf.py monitor
```

## ⚙️ Cấu Hình

Chỉnh sửa `sdkconfig` hoặc sử dụng `idf.py menuconfig` để cấu hình:

- Tên và mật khẩu WiFi
- Địa chỉ và thông tin xác thực của broker MQTT
- Các thông số cảm biến (địa chỉ I2C, khoảng thời gian khảo sát)
- Các chân GPIO của relay
- Cài đặt cập nhật OTA

## 🚀 Bắt Đầu Nhanh

1. **Điều Kiện Tiên Quyết**: ESP-IDF v5.0+, CMake 3.16+
2. **Cài Đặt ESP-IDF**: Làm theo [tài liệu chính thức](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/)
3. **Sao Chép & Xây Dựng**:

   ```bash
   git clone <repository_url>
   cd IoT_V1
   idf.py build
   ```

4. **Nạp Thiết Bị**: Kết nối ESP32 và chạy `idf.py flash`
5. **Giám Sát**: Sử dụng `idf.py monitor` để xem nhật ký

## 📝 An Toàn Luồng

Ứng dụng sử dụng các mutex của FreeRTOS để bảo vệ tài nguyên được chia sẻ:

- `sensor_data_mutex`: Bảo vệ các cờ nhiệt độ, độ ẩm và giá trị hợp lệ của cảm biến
- Đảm bảo truy cập an toàn luồng trên các nhiệm vụ WiFi, MQTT và thu thập cảm biến
- Tránh điều kiện tranh chấp và đảm bảo tính toàn vẹn dữ liệu trong môi trường đa nhiệm
