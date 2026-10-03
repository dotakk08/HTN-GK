# ssd1306 (ESP-IDF component)

Driver SSD1306 qua I2C, dùng driver mới `i2c_master` (`esp_driver_i2c`).

Dùng: copy thư mục `components/ssd1306` vào project của bạn (hoặc thêm vào `EXTRA_COMPONENT_DIRS`).
Tạo I2C bus, gọi `ssd1306_new()`, vẽ, rồi `ssd1306_flush()`. Xem `main/main.c`.
Có thể dùng chung bus I2C với các thiết bị khác vì thư viện chỉ add device vào bus bạn truyền vào.
