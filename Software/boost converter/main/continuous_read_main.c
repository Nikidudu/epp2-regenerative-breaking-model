/*
 * SPDX-FileCopyrightText: 2021-2022 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include <string.h>
#include <stdio.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "esp_err.h"
#include "driver/ledc.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include <math.h>
#include "freertos/semphr.h"
#include "esp_adc/adc_continuous.h"

#define EXAMPLE_ADC_UNIT                    ADC_UNIT_1  
#define _EXAMPLE_ADC_UNIT_STR(unit)         #unit
#define EXAMPLE_ADC_UNIT_STR(unit)          _EXAMPLE_ADC_UNIT_STR(unit)
#define EXAMPLE_ADC_CONV_MODE               ADC_CONV_SINGLE_UNIT_1
#define EXAMPLE_ADC_ATTEN                   ADC_ATTEN_DB_12
#define EXAMPLE_ADC_BIT_WIDTH               SOC_ADC_DIGI_MAX_BITWIDTH

#if CONFIG_IDF_TARGET_ESP32 || CONFIG_IDF_TARGET_ESP32S2
#define EXAMPLE_ADC_OUTPUT_TYPE             ADC_DIGI_OUTPUT_FORMAT_TYPE1
#define EXAMPLE_ADC_GET_CHANNEL(p_data)     ((p_data)->type1.channel)
#define EXAMPLE_ADC_GET_DATA(p_data)        ((p_data)->type1.data)
#else
#define EXAMPLE_ADC_OUTPUT_TYPE             ADC_DIGI_OUTPUT_FORMAT_TYPE2
#define EXAMPLE_ADC_GET_CHANNEL(p_data)     ((p_data)->type2.channel)
#define EXAMPLE_ADC_GET_DATA(p_data)        ((p_data)->type2.data)
#endif
#define ADC_NOISE_FLOOR_MV  200
#define EXAMPLE_READ_LEN                    256
/*pwm config */
#define LEDC_TIMER              LEDC_TIMER_0
#define LEDC_MODE               LEDC_HIGH_SPEED_MODE
#define LEDC_OUTPUT_IO          25 // Define the output GPIO
#define LEDC_CHANNEL            LEDC_CHANNEL_0
#define LEDC_DUTY_RES           LEDC_TIMER_13_BIT // Set duty resolution to 13 bits
#define LEDC_DUTY               (4096) // Set duty to 50%. (2 ** 13) * 50% = 4096
#define LEDC_FREQUENCY          (4000) // Frequency in Hertz. Set frequency at 4 kHz

#define SMALL_RESISTANCE 2200
#define BIG_RESISTANCE 10000
                
#define i_limit  1.80f // Integral windup limit based on max duty cycle and Ki
#define PID_KP        67.0f
#define PID_KI        12.0f
#define PID_KD        2.67f
#define VOUT  12.50f      // Volts
#define DUTY_MAX        7500.0f
#define DUTY_MIN        0.0f

#if CONFIG_IDF_TARGET_ESP32
static adc_channel_t channel[1] = {ADC_CHANNEL_6}; //GPIO 34
#else
static adc_channel_t channel[2] = {ADC_CHANNEL_2, ADC_CHANNEL_3};
#endif

static TaskHandle_t s_task_handle;
static const char* TAG = "EXAMPLE";
static adc_cali_handle_t cali_handle = NULL;

    static void example_ledc_init(void)
{
    // Prepare and then apply the LEDC PWM timer configuration
    ledc_timer_config_t ledc_timer = {
        .speed_mode       = LEDC_MODE,
        .duty_resolution  = LEDC_DUTY_RES,
        .timer_num        = LEDC_TIMER,
        .freq_hz          = LEDC_FREQUENCY,  // Set output frequency at 4 kHz
        .clk_cfg          = LEDC_AUTO_CLK
    };
    ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

    // Prepare and then apply the LEDC PWM channel configuration
    ledc_channel_config_t ledc_channel = {
        .speed_mode     = LEDC_MODE,
        .channel        = LEDC_CHANNEL,
        .timer_sel      = LEDC_TIMER,
        .intr_type      = LEDC_INTR_DISABLE,
        .gpio_num       = LEDC_OUTPUT_IO,
        .duty           = 0, // Set duty to 0%
        .hpoint         = 0
    };
    ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));
}

static bool IRAM_ATTR s_conv_done_cb(adc_continuous_handle_t handle, const adc_continuous_evt_data_t *edata, void *user_data)
{
    BaseType_t mustYield = pdFALSE;
    //Notify that ADC continuous driver has done enough number of conversions
    vTaskNotifyGiveFromISR(s_task_handle, &mustYield);

    return (mustYield == pdTRUE);
}

static void continuous_adc_init(adc_channel_t *channel, uint8_t channel_num, adc_continuous_handle_t *out_handle)
{
    adc_continuous_handle_t handle = NULL;

    adc_continuous_handle_cfg_t adc_config = {
        .max_store_buf_size = 1024,
        .conv_frame_size = EXAMPLE_READ_LEN,
    };
    ESP_ERROR_CHECK(adc_continuous_new_handle(&adc_config, &handle));

    adc_continuous_config_t dig_cfg = {
        .sample_freq_hz = 50 * 1000,
        .conv_mode = EXAMPLE_ADC_CONV_MODE,
        .format = EXAMPLE_ADC_OUTPUT_TYPE,
    };

    adc_digi_pattern_config_t adc_pattern[SOC_ADC_PATT_LEN_MAX] = {0};
    dig_cfg.pattern_num = channel_num;
    for (int i = 0; i < channel_num; i++) {
        adc_pattern[i].atten = EXAMPLE_ADC_ATTEN;
        adc_pattern[i].channel = channel[i] & 0x7;
        adc_pattern[i].unit = EXAMPLE_ADC_UNIT;
        adc_pattern[i].bit_width = EXAMPLE_ADC_BIT_WIDTH;

        ESP_LOGI(TAG, "adc_pattern[%d].atten is :%"PRIx8, i, adc_pattern[i].atten);
        ESP_LOGI(TAG, "adc_pattern[%d].channel is :%"PRIx8, i, adc_pattern[i].channel);
        ESP_LOGI(TAG, "adc_pattern[%d].unit is :%"PRIx8, i, adc_pattern[i].unit);
    }
    dig_cfg.adc_pattern = adc_pattern;
    ESP_ERROR_CHECK(adc_continuous_config(handle, &dig_cfg));

    *out_handle = handle;
}

void app_main(void)
{   
    float parallel_voltage_pd_multiplier = (float)(SMALL_RESISTANCE + BIG_RESISTANCE) / SMALL_RESISTANCE; // Calculate the multiplier for parallel voltage measurement
    esp_err_t ret;
   // esp_log_set_vprintf(NULL);
    uint32_t ret_num = 0;

    uint8_t result[EXAMPLE_READ_LEN] = {0};

    float Voutput = VOUT;

    memset(result, 0xcc, EXAMPLE_READ_LEN);

    adc_cali_line_fitting_config_t cali_config = {
        .unit_id = EXAMPLE_ADC_UNIT,
        .atten = EXAMPLE_ADC_ATTEN,
        .bitwidth = EXAMPLE_ADC_BIT_WIDTH,
    };

    float pid_integral   = 0.0f;
    float pid_prev_measurement = 0.0f;
    float dt = 0.001f;
    int   targetDutyCycle           = 0;
    int64_t last_loop_time = esp_timer_get_time();
    int64_t current_time;

    example_ledc_init();
    esp_err_t retefuse = adc_cali_create_scheme_line_fitting(&cali_config, &cali_handle);
    if (retefuse == ESP_OK) {
        ESP_LOGI(TAG, "Calibration Success!");
    } else {
        ESP_LOGW(TAG, "Calibration not supported or eFuse not burnt.");
    }
    s_task_handle = xTaskGetCurrentTaskHandle();

    adc_continuous_handle_t handle = NULL;
    continuous_adc_init(channel, sizeof(channel) / sizeof(adc_channel_t), &handle);

    adc_continuous_evt_cbs_t cbs = {
        .on_conv_done = s_conv_done_cb,
    };
    ESP_ERROR_CHECK(adc_continuous_register_event_callbacks(handle, &cbs, NULL));
    ESP_ERROR_CHECK(adc_continuous_start(handle));
    ESP_LOGI(TAG, "i_limit: %.2f", i_limit);

    while (1) {

        /**
         * This is to show you the way to use the ADC continuous mode driver event callback.
         * This `ulTaskNotifyTake` will block when the data processing in the task is fast.
         * However in this example, the data processing (print) is slow, so you barely block here.
         *
         * Without using this event callback (to notify this task), you can still just call
         * `adc_continuous_read()` here in a loop, with/without a certain block timeout.
         */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

       // char unit[] = EXAMPLE_ADC_UNIT_STR(EXAMPLE_ADC_UNIT);

        while (1) {
            ret = adc_continuous_read(handle, result, EXAMPLE_READ_LEN, &ret_num, 0);
            if (ret == ESP_OK) {
              //  ESP_LOGI("TASK", "ret is %x, ret_num is %"PRIu32" bytes", ret, ret_num);
                uint32_t sum_raw = 0;

                current_time = esp_timer_get_time();
                dt = (current_time - last_loop_time) / 1000000.0f; // Convert microseconds to seconds
                last_loop_time = current_time;
                int sample_count = 0;
                int calibrated_voltage = 0;
                for (int i = 0; i < ret_num; i += SOC_ADC_DIGI_RESULT_BYTES) {
                    adc_digi_output_data_t *p = (adc_digi_output_data_t*)&result[i];
        

                    sum_raw += EXAMPLE_ADC_GET_DATA(p);
                    sample_count++;
                    
                }
                if (sample_count > 0) {
                    uint32_t average_raw = sum_raw / sample_count; // This is your "Multisampled" value    
                    adc_cali_raw_to_voltage(cali_handle, average_raw, &calibrated_voltage);

                }
                

                float node_voltage;
                float pid_output;
               // float targetDutyCycle;
                if (calibrated_voltage < ADC_NOISE_FLOOR_MV) {
                    calibrated_voltage = 0;
                    node_voltage = 0.0f;
                    pid_integral = 0.0f; // Reset integral term if voltage is at noise floor
                    pid_prev_measurement = 0.0f; // Reset previous measurement
                    targetDutyCycle = 0; // Set to max duty cycle to try to increase voltage
                    pid_output = 0.0f;

                } else {
                    node_voltage = ((calibrated_voltage ) * parallel_voltage_pd_multiplier / 1000.0);
                    float error = Voutput - node_voltage;
                    pid_integral += error * dt;
                    
                    if (pid_integral >  i_limit) pid_integral =  i_limit;
                    if (pid_integral < -i_limit) pid_integral = -i_limit;
                    
                    float derivative = (pid_prev_measurement - node_voltage) / dt;
                    pid_prev_measurement = node_voltage;
                    pid_output     = PID_KP * error + PID_KI * pid_integral + PID_KD * derivative;
                    targetDutyCycle += pid_output;
                    if (targetDutyCycle > DUTY_MAX) targetDutyCycle = DUTY_MAX;
                    if (targetDutyCycle < DUTY_MIN) targetDutyCycle = DUTY_MIN;



                    //targetDutyCycle = round((1.0-(node_voltage / Voutput))*8191.0); // Calculate duty cycle based on voltage
                    
                }
                                   
                ESP_ERROR_CHECK(ledc_set_duty(LEDC_MODE, LEDC_CHANNEL, (int)targetDutyCycle)); 
                ESP_ERROR_CHECK(ledc_update_duty(LEDC_MODE, LEDC_CHANNEL));
               // ESP_LOGI(TAG, "Calibrated Voltage: %d mV , Node Voltage: %.2f V, with PWM Duty Cycle: %d", calibrated_voltage, node_voltage, targetDutyCycle);
                ESP_LOGI(TAG, "CURRENT TIME: %"PRIu64" NODE VOLTAGE: %.2f V, PWM: %d, PID: %.2f, pid_integral: %.2f", current_time, node_voltage, (int)targetDutyCycle, pid_output, pid_integral);
                vTaskDelay(1);
            } else if (ret == ESP_ERR_TIMEOUT) {
                //We try to read `EXAMPLE_READ_LEN` until API returns timeout, which means there's no available data
                break;
            }
        }
    }

    ESP_ERROR_CHECK(adc_continuous_stop(handle));
    ESP_ERROR_CHECK(adc_continuous_deinit(handle));
}
