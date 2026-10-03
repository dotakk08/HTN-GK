#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ssd1306 *ssd1306_handle_t;

typedef enum {
    SSD1306_COLOR_BLACK  = 0,
    SSD1306_COLOR_WHITE  = 1,
    SSD1306_COLOR_INVERT = 2,
} ssd1306_color_t;

typedef struct {
    i2c_master_bus_handle_t bus;  /*!< I2C bus đã tạo bằng i2c_new_master_bus() */
    uint8_t  addr;                /*!< Địa chỉ I2C 7-bit: 0x3C hoặc 0x3D */
    uint32_t scl_speed_hz;        /*!< Tốc độ SCL, vd 400000 */
    uint8_t  width;               /*!< Thường 128 */
    uint8_t  height;              /*!< 64 hoặc 32 */
    bool     flip_horizontal;     /*!< Lật ngang */
    bool     flip_vertical;       /*!< Lật dọc (xoay 180° khi bật cả hai) */
} ssd1306_config_t;

#define SSD1306_I2C_CONFIG_DEFAULT(bus_handle) { \
    .bus = (bus_handle), .addr = 0x3C, .scl_speed_hz = 400000, \
    .width = 128, .height = 64, .flip_horizontal = false, .flip_vertical = false }

/** Khởi tạo màn hình, cấp phát framebuffer. */
esp_err_t ssd1306_new(const ssd1306_config_t *cfg, ssd1306_handle_t *out);
/** Giải phóng tài nguyên (không xoá I2C bus của người dùng). */
esp_err_t ssd1306_del(ssd1306_handle_t h);

/** Đẩy framebuffer ra màn hình. */
esp_err_t ssd1306_flush(ssd1306_handle_t h);

/* Điều khiển phần cứng */
esp_err_t ssd1306_set_contrast(ssd1306_handle_t h, uint8_t contrast);
esp_err_t ssd1306_set_power(ssd1306_handle_t h, bool on);
esp_err_t ssd1306_set_invert(ssd1306_handle_t h, bool invert);

/* Vẽ lên framebuffer (cần gọi ssd1306_flush để hiển thị) */
void ssd1306_clear(ssd1306_handle_t h);
void ssd1306_fill(ssd1306_handle_t h, ssd1306_color_t color);
void ssd1306_draw_pixel(ssd1306_handle_t h, int x, int y, ssd1306_color_t color);
void ssd1306_draw_line(ssd1306_handle_t h, int x0, int y0, int x1, int y1, ssd1306_color_t color);
void ssd1306_draw_rect(ssd1306_handle_t h, int x, int y, int w, int hgt, ssd1306_color_t color);
void ssd1306_fill_rect(ssd1306_handle_t h, int x, int y, int w, int hgt, ssd1306_color_t color);
void ssd1306_draw_circle(ssd1306_handle_t h, int cx, int cy, int r, ssd1306_color_t color);
/** Bitmap 1bpp, mỗi byte là 8 pixel ngang (MSB = pixel trái), mỗi hàng làm tròn lên theo byte. */
void ssd1306_draw_bitmap(ssd1306_handle_t h, int x, int y, const uint8_t *bmp, int w, int hgt, ssd1306_color_t color);

/** Font 5x7, scale >= 1. Mỗi ký tự chiếm (6*scale) x (8*scale) pixel. Hỗ trợ '\n'. */
void ssd1306_draw_char(ssd1306_handle_t h, int x, int y, char c, int scale, ssd1306_color_t color);
void ssd1306_draw_string(ssd1306_handle_t h, int x, int y, const char *str, int scale, ssd1306_color_t color);
void ssd1306_printf(ssd1306_handle_t h, int x, int y, int scale, ssd1306_color_t color, const char *fmt, ...)
    __attribute__((format(printf, 6, 7)));

int ssd1306_get_width(ssd1306_handle_t h);
int ssd1306_get_height(ssd1306_handle_t h);

#ifdef __cplusplus
}
#endif
