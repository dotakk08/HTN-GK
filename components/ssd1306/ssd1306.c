#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_log.h"
#include "esp_check.h"
#include "ssd1306.h"
#include "ssd1306_font.h"

static const char *TAG = "ssd1306";

#define CTRL_CMD   0x00
#define CTRL_DATA  0x40
#define I2C_TIMEOUT_MS 100

struct ssd1306 {
    i2c_master_dev_handle_t dev;
    uint8_t width, height, pages;
    uint8_t *fb;      /* width * pages */
    uint8_t *txbuf;   /* 1 + width */
};

static esp_err_t send_cmds(ssd1306_handle_t h, const uint8_t *cmds, size_t n)
{
    uint8_t buf[16];
    ESP_RETURN_ON_FALSE(n < sizeof(buf), ESP_ERR_INVALID_SIZE, TAG, "cmd too long");
    buf[0] = CTRL_CMD;
    memcpy(&buf[1], cmds, n);
    return i2c_master_transmit(h->dev, buf, n + 1, I2C_TIMEOUT_MS);
}

static inline esp_err_t send_cmd1(ssd1306_handle_t h, uint8_t c) { return send_cmds(h, &c, 1); }

esp_err_t ssd1306_new(const ssd1306_config_t *cfg, ssd1306_handle_t *out)
{
    ESP_RETURN_ON_FALSE(cfg && out && cfg->bus, ESP_ERR_INVALID_ARG, TAG, "invalid arg");
    ESP_RETURN_ON_FALSE(cfg->width > 0 && cfg->width <= 128, ESP_ERR_INVALID_ARG, TAG, "bad width");
    ESP_RETURN_ON_FALSE(cfg->height == 64 || cfg->height == 32, ESP_ERR_INVALID_ARG, TAG, "height must be 32 or 64");

    esp_err_t ret = i2c_master_probe(cfg->bus, cfg->addr, I2C_TIMEOUT_MS);
    ESP_RETURN_ON_ERROR(ret, TAG, "no device at 0x%02X", cfg->addr);

    ssd1306_handle_t h = calloc(1, sizeof(*h));
    ESP_RETURN_ON_FALSE(h, ESP_ERR_NO_MEM, TAG, "no mem");
    h->width = cfg->width;
    h->height = cfg->height;
    h->pages = cfg->height / 8;
    h->fb = calloc(h->width * h->pages, 1);
    h->txbuf = malloc(1 + h->width);
    ESP_GOTO_ON_FALSE(h->fb && h->txbuf, ESP_ERR_NO_MEM, err, TAG, "no mem");

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = cfg->addr,
        .scl_speed_hz = cfg->scl_speed_hz ? cfg->scl_speed_hz : 400000,
    };
    ESP_GOTO_ON_ERROR(i2c_master_bus_add_device(cfg->bus, &dev_cfg, &h->dev), err, TAG, "add device failed");

    const uint8_t init[] = {
        0xAE,                               /* display off */
        0xD5, 0x80,                         /* clock div / osc freq */
        0xA8, (uint8_t)(h->height - 1),     /* multiplex ratio */
        0xD3, 0x00,                         /* display offset */
        0x40,                               /* start line = 0 */
        0x8D, 0x14,                         /* charge pump on */
        0x20, 0x00,                         /* horizontal addressing mode */
        cfg->flip_horizontal ? 0xA0 : 0xA1, /* segment remap */
        cfg->flip_vertical   ? 0xC0 : 0xC8, /* COM scan direction */
        0xDA, (uint8_t)(h->height == 64 ? 0x12 : 0x02),
        0x81, 0xCF,                         /* contrast */
        0xD9, 0xF1,                         /* pre-charge */
        0xDB, 0x40,                         /* VCOMH deselect */
        0xA4,                               /* resume from RAM */
        0xA6,                               /* normal (non-inverted) */
        0xAF,                               /* display on */
    };
    /* Gửi từng lệnh để mỗi transaction ngắn */
    for (size_t i = 0; i < sizeof(init);) {
        size_t n = 1;
        if (init[i] == 0xD5 || init[i] == 0xA8 || init[i] == 0xD3 || init[i] == 0x8D ||
            init[i] == 0x20 || init[i] == 0xDA || init[i] == 0x81 || init[i] == 0xD9 || init[i] == 0xDB) {
            n = 2;
        }
        ESP_GOTO_ON_ERROR(send_cmds(h, &init[i], n), err, TAG, "init failed");
        i += n;
    }

    ESP_GOTO_ON_ERROR(ssd1306_flush(h), err, TAG, "first flush failed");
    *out = h;
    return ESP_OK;

err:
    ssd1306_del(h);
    return ret ? ret : ESP_FAIL;
}

esp_err_t ssd1306_del(ssd1306_handle_t h)
{
    if (!h) return ESP_OK;
    if (h->dev) i2c_master_bus_rm_device(h->dev);
    free(h->fb);
    free(h->txbuf);
    free(h);
    return ESP_OK;
}

esp_err_t ssd1306_flush(ssd1306_handle_t h)
{
    ESP_RETURN_ON_FALSE(h, ESP_ERR_INVALID_ARG, TAG, "null handle");
    const uint8_t win[] = { 0x21, 0, (uint8_t)(h->width - 1), 0x22, 0, (uint8_t)(h->pages - 1) };
    ESP_RETURN_ON_ERROR(send_cmds(h, &win[0], 3), TAG, "col addr");
    ESP_RETURN_ON_ERROR(send_cmds(h, &win[3], 3), TAG, "page addr");
    for (uint8_t p = 0; p < h->pages; p++) {
        h->txbuf[0] = CTRL_DATA;
        memcpy(&h->txbuf[1], &h->fb[p * h->width], h->width);
        ESP_RETURN_ON_ERROR(i2c_master_transmit(h->dev, h->txbuf, 1 + h->width, I2C_TIMEOUT_MS), TAG, "data");
    }
    return ESP_OK;
}

esp_err_t ssd1306_set_contrast(ssd1306_handle_t h, uint8_t contrast)
{
    const uint8_t c[] = { 0x81, contrast };
    return send_cmds(h, c, 2);
}
esp_err_t ssd1306_set_power(ssd1306_handle_t h, bool on) { return send_cmd1(h, on ? 0xAF : 0xAE); }
esp_err_t ssd1306_set_invert(ssd1306_handle_t h, bool inv) { return send_cmd1(h, inv ? 0xA7 : 0xA6); }

/* ---------------- Drawing ---------------- */

void ssd1306_fill(ssd1306_handle_t h, ssd1306_color_t color)
{
    memset(h->fb, color == SSD1306_COLOR_WHITE ? 0xFF : 0x00, h->width * h->pages);
}
void ssd1306_clear(ssd1306_handle_t h) { ssd1306_fill(h, SSD1306_COLOR_BLACK); }

void ssd1306_draw_pixel(ssd1306_handle_t h, int x, int y, ssd1306_color_t color)
{
    if (x < 0 || y < 0 || x >= h->width || y >= h->height) return;
    uint8_t *p = &h->fb[(y >> 3) * h->width + x];
    uint8_t m = 1u << (y & 7);
    switch (color) {
    case SSD1306_COLOR_WHITE:  *p |= m;  break;
    case SSD1306_COLOR_BLACK:  *p &= ~m; break;
    case SSD1306_COLOR_INVERT: *p ^= m;  break;
    }
}

void ssd1306_draw_line(ssd1306_handle_t h, int x0, int y0, int x1, int y1, ssd1306_color_t color)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        ssd1306_draw_pixel(h, x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void ssd1306_draw_rect(ssd1306_handle_t h, int x, int y, int w, int hg, ssd1306_color_t color)
{
    if (w <= 0 || hg <= 0) return;
    ssd1306_draw_line(h, x, y, x + w - 1, y, color);
    ssd1306_draw_line(h, x, y + hg - 1, x + w - 1, y + hg - 1, color);
    ssd1306_draw_line(h, x, y, x, y + hg - 1, color);
    ssd1306_draw_line(h, x + w - 1, y, x + w - 1, y + hg - 1, color);
}

void ssd1306_fill_rect(ssd1306_handle_t h, int x, int y, int w, int hg, ssd1306_color_t color)
{
    for (int j = 0; j < hg; j++)
        for (int i = 0; i < w; i++)
            ssd1306_draw_pixel(h, x + i, y + j, color);
}

void ssd1306_draw_circle(ssd1306_handle_t h, int cx, int cy, int r, ssd1306_color_t color)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        ssd1306_draw_pixel(h, cx + x, cy + y, color); ssd1306_draw_pixel(h, cx - x, cy + y, color);
        ssd1306_draw_pixel(h, cx + x, cy - y, color); ssd1306_draw_pixel(h, cx - x, cy - y, color);
        ssd1306_draw_pixel(h, cx + y, cy + x, color); ssd1306_draw_pixel(h, cx - y, cy + x, color);
        ssd1306_draw_pixel(h, cx + y, cy - x, color); ssd1306_draw_pixel(h, cx - y, cy - x, color);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

void ssd1306_draw_bitmap(ssd1306_handle_t h, int x, int y, const uint8_t *bmp, int w, int hg, ssd1306_color_t color)
{
    int bpr = (w + 7) / 8;
    for (int j = 0; j < hg; j++)
        for (int i = 0; i < w; i++)
            if (bmp[j * bpr + (i >> 3)] & (0x80 >> (i & 7)))
                ssd1306_draw_pixel(h, x + i, y + j, color);
}

void ssd1306_draw_char(ssd1306_handle_t h, int x, int y, char c, int scale, ssd1306_color_t color)
{
    if (scale < 1) scale = 1;
    if (c < 32 || c > 126) c = '?';
    const uint8_t *g = ssd1306_font5x7[c - 32];
    for (int col = 0; col < 5; col++)
        for (int row = 0; row < 7; row++)
            if (g[col] & (1u << row))
                ssd1306_fill_rect(h, x + col * scale, y + row * scale, scale, scale, color);
}

void ssd1306_draw_string(ssd1306_handle_t h, int x, int y, const char *s, int scale, ssd1306_color_t color)
{
    if (scale < 1) scale = 1;
    int cx = x, cy = y;
    for (; *s; s++) {
        if (*s == '\n') { cx = x; cy += 8 * scale; continue; }
        if (*s == '\r') continue;
        ssd1306_draw_char(h, cx, cy, *s, scale, color);
        cx += 6 * scale;
    }
}

void ssd1306_printf(ssd1306_handle_t h, int x, int y, int scale, ssd1306_color_t color, const char *fmt, ...)
{
    char buf[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ssd1306_draw_string(h, x, y, buf, scale, color);
}

int ssd1306_get_width(ssd1306_handle_t h)  { return h->width; }
int ssd1306_get_height(ssd1306_handle_t h) { return h->height; }
