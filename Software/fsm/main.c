#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

/* ── Pin definitions ───────────────────────────────────────────────── */
#define FORCE_SENSOR_PIN    ADC_CHANNEL_3   // GPIO4  on ESP32 = ADC1_CH3
#define PWM_OUTPUT_PIN      2               // GPIO2
#define DIGITAL_OUT_PIN     1               // GPIO1
#define BUTTON_OUT_PIN      18              // GPIO18
#define BUTTON_IN_PIN       17              // GPIO17
#define RELAY_PIN           16              // GPIO16

/* ── Thresholds / constants ────────────────────────────────────────── */
#define FORCE_THRESHOLD     1000            // raw ADC counts (0–4095)
#define ADC_MAX_RAW         4095
#define PWM_MAX_DUTY        255             // 8-bit PWM  (0–255)

/* ── LEDC (PWM) config ─────────────────────────────────────────────── */
#define LEDC_TIMER          LEDC_TIMER_0
#define LEDC_MODE           LEDC_LOW_SPEED_MODE
#define LEDC_CHANNEL        LEDC_CHANNEL_0
#define LEDC_DUTY_RES       LEDC_TIMER_8_BIT   // 8-bit → 0-255
#define LEDC_FREQUENCY      5000                // 5 kHz

static const char *TAG = "FORCE_CTRL";

/* ──────────────────────────────────────────────────────────────────── */
/*  Helpers                                                             */
/* ──────────────────────────────────────────────────────────────────── */

/** Linear map, identical to Arduino map() */
static inline int map_value(int x, int in_min, int in_max, int out_min, int out_max)
{
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
}

/** Clamp an int between lo and hi */
static inline int constrain_value(int x, int lo, int hi)
{
    if (x < lo) return lo;
    if (x > hi) return hi;
    return x;
}

/* ──────────────────────────────────────────────────────────────────── */
/*  Peripheral initialisation                                           */
/* ──────────────────────────────────────────────────────────────────── */

static void gpio_init(void)
{
    /* outputs */
    gpio_config_t out_cfg = {
        .pin_bit_mask = (1ULL << DIGITAL_OUT_PIN) |
                        (1ULL << BUTTON_OUT_PIN)  |
                        (1ULL << RELAY_PIN),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&out_cfg);

    /* inputs */
    gpio_config_t in_cfg = {
        .pin_bit_mask = (1ULL << BUTTON_IN_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&in_cfg);

    /* match Arduino setup() initial states */
    gpio_set_level(DIGITAL_OUT_PIN, 0);   // LOW
    gpio_set_level(BUTTON_OUT_PIN,  1);   // HIGH
    gpio_set_level(RELAY_PIN,       1);   // HIGH
}

static void ledc_pwm_init(void)
{
    ledc_timer_config_t timer = {
        .speed_mode      = LEDC_MODE,
        .duty_resolution = LEDC_DUTY_RES,
        .timer_num       = LEDC_TIMER,
        .freq_hz         = LEDC_FREQUENCY,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    ledc_channel_config_t ch = {
        .speed_mode = LEDC_MODE,
        .channel    = LEDC_CHANNEL,
        .timer_sel  = LEDC_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = PWM_OUTPUT_PIN,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
}

static adc_oneshot_unit_handle_t adc_init(void)
{
    adc_oneshot_unit_handle_t handle;

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id  = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unit_cfg, &handle));

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = ADC_ATTEN_DB_12,        // up to ~3.1 V, matches ADC_11db
        .bitwidth = ADC_BITWIDTH_12,        // 0–4095
    };
    ESP_ERROR_CHECK(adc_oneshot_config_channel(handle, FORCE_SENSOR_PIN, &chan_cfg));

    return handle;
}

/* ──────────────────────────────────────────────────────────────────── */
/*  PWM helper                                                          */
/* ──────────────────────────────────────────────────────────────────── */

static void set_pwm(int duty)   // duty: 0–255
{
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, (uint32_t)duty));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_CHANNEL));
}

/* ──────────────────────────────────────────────────────────────────── */
/*  Main task  (replaces loop())                                        */
/* ──────────────────────────────────────────────────────────────────── */

void app_main(void)
{
    gpio_init();
    ledc_pwm_init();
    adc_oneshot_unit_handle_t adc_handle = adc_init();

    bool break_flag = true;

    while (1) {
        /* ── 1. Read force sensor ──────────────────────────────────── */
        int analog_reading = 0;
        ESP_ERROR_CHECK(adc_oneshot_read(adc_handle, FORCE_SENSOR_PIN, &analog_reading));

        /* ── 2. Map ADC → PWM duty (0-4095 → 0-255) ───────────────── */
        int pwm_value = map_value(analog_reading, 0, ADC_MAX_RAW, 0, PWM_MAX_DUTY);
        pwm_value = constrain_value(pwm_value, 0, PWM_MAX_DUTY);

        /* ── 3. Button → relay + break_flag ───────────────────────── */
        if (gpio_get_level(BUTTON_IN_PIN) == 1) {
            gpio_set_level(RELAY_PIN, 0);   // LOW
            break_flag = false;
        } else {
            gpio_set_level(RELAY_PIN, 1);   // HIGH
            break_flag = true;
        }

        /* ── 4. Force threshold gate ───────────────────────────────── */
        if ((analog_reading <= FORCE_THRESHOLD) && break_flag) {
            gpio_set_level(DIGITAL_OUT_PIN, 0);
            set_pwm(0);
        } else {
            gpio_set_level(DIGITAL_OUT_PIN, 1);
            set_pwm(pwm_value);
        }

        /* ── 5. Serial logging ─────────────────────────────────────── */
        bool output_suppressed = (analog_reading <= FORCE_THRESHOLD) && break_flag;
        ESP_LOGI(TAG, "Force = %4d | PWM = %3d | OUT = %-4s | break_flag = %s",
                 analog_reading,
                 output_suppressed ? 0 : pwm_value,
                 output_suppressed ? "LOW" : "HIGH",
                 break_flag        ? "true" : "false");

        vTaskDelay(pdMS_TO_TICKS(20));  // 20 ms, same as Arduino delay(20)
    }
}