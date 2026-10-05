// #include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
// #include "freertos/task.h"
// #include "freertos/queue.h"
#include "esp_log.h"
#include "driver/gpio.h"
// #include "driver/i2c_master.h"
#include "esp_adc/adc_oneshot.h"
#include "led_strip.h"
#include "ssd1306.h"
// #include <stdbool.h>

static const char *TAG = "HE_THONG_BAO_CHAY";

/* --- KHAI BÁO CHÂN PHẦN CỨNG --- */
#define PIN_FLAME_SENSOR GPIO_NUM_2
#define PIN_BUZZER       GPIO_NUM_3  
#define PIN_LED_STRIP    GPIO_NUM_5
#define SO_BONG_LED      8  // Số lượng LED có thể điều khiển được trên đèn (VD 8/8 )
  
/* OLED SSD1306 (I2C) */
#define PIN_OLED_SDA     GPIO_NUM_6
#define PIN_OLED_SCL     GPIO_NUM_7
#define OLED_ADDR        0x3C

/* CẢM BIẾN MQ (ADC) - Gộp chung Gas và CO */
#define GPIO_ADC_MQ      ADC_CHANNEL_0
#define V_REF            3.3f
#define ADC_LEVEL        4095.0f      // 2^12 - 1
#define MQ_RO            1.0f         // Ro trong không khí sạch (kΩ)
#define KHI_NGUONG_BAO   5.0f        // >= ngưỡng này thì báo động (ppm)
#define KHI_NGUONG_TAT   4.0f        // <= ngưỡng này thì hết báo động (chống nhấp nháy)

/* --- KIỂU DỮ LIỆU --- */
typedef enum {
    eCamBienLua,
    eCamBienMQ9 // Thay thế cho eCamBienGas và eCamBienCO
} NguonCamBien_t;

typedef struct {
    NguonCamBien_t eNguon;
    bool bNguyHiem;   // 1 = Nguy hiểm, 0 = Bình thường
    float   fGiaTri;     // Nồng độ PPM
} ThongTinCamBien_t;

/* --- BIẾN TOÀN CỤC --- */
static QueueHandle_t xSensorQueue = NULL;
static led_strip_handle_t led_strip;
static adc_oneshot_unit_handle_t adc_handle;
static ssd1306_handle_t oled = NULL;

/* ========================================================================= *
 * BUZZER
 * ========================================================================= */
static void DieuKhienBuzzer(bool is_on)
{
    gpio_set_level(PIN_BUZZER, is_on);
}

/* ========================================================================= *
 * OLED
 * ========================================================================= */
static void KhoiTaoOLED(void)
{
    i2c_master_bus_handle_t bus;
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_OLED_SDA,
        .scl_io_num = PIN_OLED_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    ssd1306_config_t cfg = SSD1306_I2C_CONFIG_DEFAULT(bus);
    cfg.addr = OLED_ADDR;
    if (ssd1306_new(&cfg, &oled) != ESP_OK) {
        oled = NULL;
        ESP_LOGE(TAG, "Khong tim thay OLED SSD1306 @0x%02X", OLED_ADDR);
    }
}

static void CapNhatManHinhOLED(bool co_lua, bool co_khi, float ppm)
{
    static int last_alarm = -1;
    bool alarm = co_lua || co_khi;

    printf("[OLED] %s | Lua: %d | Khi Doc: %d (%.1f ppm)\n",
           alarm ? "NGUY HIEM" : "AN TOAN", co_lua, co_khi, ppm);

    if (!oled) return;

    ssd1306_clear(oled);
    ssd1306_draw_string(oled, 0, 0, "HE THONG BAO CHAY", 1, SSD1306_COLOR_WHITE);
    ssd1306_draw_line(oled, 0, 10, 127, 10, SSD1306_COLOR_WHITE);

    ssd1306_draw_string(oled, 0, 14, alarm ? "NGUY HIEM!" : "AN TOAN", 2, SSD1306_COLOR_WHITE);

    ssd1306_printf(oled, 0, 36, 1, SSD1306_COLOR_WHITE, "LUA     : %s", co_lua ? "PHAT HIEN" : "Khong");
    ssd1306_printf(oled, 0, 50, 1, SSD1306_COLOR_WHITE, "KHI DOC : %.1f ppm%s", ppm, co_khi ? " !" : "");

    ssd1306_flush(oled);

    if (alarm != last_alarm) {          
        ssd1306_set_invert(oled, alarm);
        last_alarm = alarm;
    }
}

/* ========================================================================= *
 * LED STRIP
 * ========================================================================= */
static void KhaiBaoLedStrip(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = PIN_LED_STRIP,
        .max_leds = SO_BONG_LED,
    };
    led_strip_rmt_config_t rmt_config = {
        .resolution_hz = 10 * 1000 * 1000,
        .flags.with_dma = false,
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
    led_strip_clear(led_strip);
}

static void DieuKhienLedStrip_That(int che_do)
{
    if (che_do == 0) {
        led_strip_clear(led_strip);
        return;
    }
    uint8_t r = 0, g = 0, b = 0;
    if (che_do == 1) g = 50;                 // Xanh lá (bình thường)
    else if (che_do == 2) r = 255;           // Đỏ chói (báo động)

    for (int i = 0; i < SO_BONG_LED; i++) led_strip_set_pixel(led_strip, i, r, g, b);
    led_strip_refresh(led_strip);
}

/* ========================================================================= *
 * CẢM BIẾN KHÍ (ADC)
 * ========================================================================= */
static void adc_install_instance(void)
{
    adc_oneshot_unit_init_cfg_t init_config = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle));

    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_12,
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle, GPIO_ADC_MQ, &config));
}

static float TinhNongDoKhi(int raw_data)
{
    if (raw_data <= 0) return 0.0f;
    if (raw_data >= 4090) return 2000.0f;

    float v_data = (raw_data / ADC_LEVEL) * V_REF;
    float Rs = 10.0f * ((V_REF / v_data) - 1.0f);        
    float ty_le_Rs_Ro = Rs / MQ_RO;
    return 599.65f * powf(ty_le_Rs_Ro, -2.244f);         
}

/* ========================================================================= *
 * TASK 1: ĐỌC CẢM BIẾN LỬA (digital, quét 100 ms)
 * ========================================================================= */
static void vTaskDocCamBienLua(void *pvParameters) 
{   

    bool old_flame = 1;
    ThongTinCamBien_t dulieuGui = { .eNguon = eCamBienLua };

    for (;;) {
        bool current_flame = gpio_get_level(PIN_FLAME_SENSOR);   // 0 = có lửa
        if (current_flame != old_flame) {
            dulieuGui.eNguon = eCamBienLua;
            dulieuGui.bNguyHiem = (current_flame == 0); // 0 = có sự cố
            // [SỬA Ở ĐÂY]: Kiểm tra xem đẩy vào Queue có thành công (pdPASS) không
            // Nếu Queue đầy bị thất bại, vòng lặp sau nó sẽ tự động gửi lại.
            if (xQueueSend(xSensorQueue, &dulieuGui, 0) == pdPASS) {
                old_flame = current_flame; // Chỉ nhớ trạng thái mới khi đã báo cáo xong
            }
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
} // Đọc cảm biến lửa và gửi vào Queue 

/* ========================================================================= *
 * TASK 2: ĐỌC CẢM BIẾN KHÍ ĐỘC (ADC, mỗi 1 giây)
 * ========================================================================= */
static void vTaskDocCamBienKhi(void *pvParameters)
{
    adc_install_instance();

    int raw = 0;
    bool co_nguy_hiem = 0;
    ThongTinCamBien_t dulieuGui = { .eNguon = eCamBienMQ9 };

    for (;;) {
        if (adc_oneshot_read(adc_handle, GPIO_ADC_MQ, &raw) == ESP_OK) {
            float ppm = TinhNongDoKhi(raw);

            if (ppm >= KHI_NGUONG_BAO) co_nguy_hiem = 1;
            else if (ppm <= KHI_NGUONG_TAT) co_nguy_hiem = 0;

            dulieuGui.fGiaTri = ppm;
            dulieuGui.bNguyHiem = co_nguy_hiem;
            xQueueSend(xSensorQueue, &dulieuGui, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ========================================================================= *
 * TASK 3: XỬ LÝ TRUNG TÂM + HIỂN THỊ
 * ========================================================================= */
static void vTaskXuLyTrungTam(void *pvParameters)
{
    gpio_set_direction(PIN_BUZZER, GPIO_MODE_OUTPUT);

    ThongTinCamBien_t dulieuNhan;
    uint8_t flag_lua = 0, flag_khi = 0;
    float ppm_khi = 0.0f;
    uint8_t toggle_led_do = 0;

    int thoi_gian_giu_canh_bao = 0; 
    uint8_t nho_lua = 0, nho_khi = 0; 

    CapNhatManHinhOLED(0, 0, 0.0f);

    for (;;) {
        // 1. RÚT SẠCH DỮ LIỆU TRONG QUEUE (Không chờ - 0 ms)
        // Dùng vòng lặp while để vét sạch các tín hiệu nhiễu/bập bùng của lửa
        // Task chỉ lấy kết quả (0 hoặc 1) cuối cùng và mới nhất.
        while (xQueueReceive(xSensorQueue, &dulieuNhan, 0) == pdPASS) {
            switch (dulieuNhan.eNguon) {
                case eCamBienLua: 
                    flag_lua = dulieuNhan.bNguyHiem; 
                    break;
                case eCamBienMQ9:  
                    flag_khi = dulieuNhan.bNguyHiem; 
                    ppm_khi = dulieuNhan.fGiaTri; 
                    break;
            }
        }

        // 2. LOGIC LƯU TRỮ VÀ ĐẾM NGƯỢC 3 GIÂY (Giữ nguyên)
        if (flag_lua || flag_khi) {
            thoi_gian_giu_canh_bao = 12; // Nạp lại 3 giây
            if (flag_lua) nho_lua = 1;
            if (flag_khi) nho_khi = 1;
        } 
        else if (thoi_gian_giu_canh_bao > 0) {
            thoi_gian_giu_canh_bao--;
        }

        if (thoi_gian_giu_canh_bao == 0) {
            nho_lua = 0;
            nho_khi = 0;
        }

        // 3. ĐIỀU KHIỂN OLED, CÒI, ĐÈN (Giữ nguyên)
        CapNhatManHinhOLED(nho_lua, nho_khi, ppm_khi);

        if (thoi_gian_giu_canh_bao > 0) {
            DieuKhienBuzzer(1);
            toggle_led_do = !toggle_led_do;
            DieuKhienLedStrip_That(toggle_led_do ? 2 : 0);   // Nháy đỏ
        } else {
            DieuKhienBuzzer(0);
            DieuKhienLedStrip_That(1);                       // Xanh lá
        }

        // 4. BỘ ĐỊNH THỜI CỨNG KHÚC CUỐI (Quan trọng nhất)
        // Ép Task luôn ngủ 250ms cho dù cảm biến có nhảy 0-1 điên cuồng đến đâu
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
void app_main(void)
{
    printf("He thong canh bao bat dau hoat dong...\n");

    KhaiBaoLedStrip();
    KhoiTaoOLED();


    xSensorQueue = xQueueCreate(1, sizeof(ThongTinCamBien_t));
    gpio_set_direction(PIN_FLAME_SENSOR, GPIO_MODE_INPUT);
    gpio_set_direction(PIN_BUZZER, GPIO_MODE_OUTPUT);


    xTaskCreate(vTaskDocCamBienLua, "Doc_Lua",         2048, NULL, 2, NULL);
    xTaskCreate(vTaskDocCamBienKhi, "Doc_Khi",         3072, NULL, 2, NULL);
    xTaskCreate(vTaskXuLyTrungTam,  "Xu_Ly_Trung_Tam", 4096, NULL, 1, NULL);
}