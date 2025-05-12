#pragma once
#include <functional>
#include <vector>

#include <driver/gpio.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_timer.h>

class PowerManager {
  private:
    esp_timer_handle_t timer_handle_;
    std::function<void(bool)> on_charging_status_changed_;
    std::function<void(bool)> on_low_battery_status_changed_;
    adc_cali_handle_t power_adc_cali_handle = NULL;

    gpio_num_t charging_pin_ = GPIO_NUM_NC;
    std::vector<uint16_t> adc_values_;
    // 初始电量不要低于LowBatteryLevel不然会提示低电量
    uint32_t battery_level_ = 50;
    bool is_charging_ = false;
    bool is_low_battery_ = false;
    int ticks_ = 0;
    const int kBatteryAdcInterval = 60;
    const int kBatteryAdcDataCount = 5;
    const int kLowBatteryLevel = 20;
    float power_value = 0;

    adc_oneshot_unit_handle_t adc_handle_;

    void CheckBatteryStatus() {
        // 如果电池电量数据不足，则读取电池电量数据
        if (adc_values_.size() < kBatteryAdcDataCount) {
            ReadBatteryAdcData();
            return;
        }

        // 如果电池电量数据充足，则每 kBatteryAdcInterval 个 tick 读取一次电池电量数据
        ticks_++;
        if (ticks_ % kBatteryAdcInterval == 0) {
            ReadBatteryAdcData();

            // 重置ticks_ 无实际意义
            if (ticks_ > 2000) {
                ticks_ = 0;
            }
        }

        // 最新电量大于平均值，则认为在充电
        uint32_t average_adc = 0;
        for (auto value : adc_values_) {
            average_adc += value;
        }
        average_adc /= adc_values_.size();

        if (is_charging_ != (power_value > average_adc)) {
            is_charging_ = power_value > average_adc;
            if (on_charging_status_changed_) {
                on_charging_status_changed_(is_charging_);
            }
        }
    }

    void ReadBatteryAdcData() {
        int adc_value;
        int voltage = 0;
        ESP_ERROR_CHECK(adc_oneshot_read(adc_handle_, POWER_ADC_CHANNEL, &adc_value));
        ESP_ERROR_CHECK(adc_cali_raw_to_voltage(power_adc_cali_handle, adc_value, &voltage));

        power_value = POWER_VOLTAGE_RATIO * (VOLT_FILTER_ALPHA * voltage + (1.0f - VOLT_FILTER_ALPHA) * voltage);

        // 将 ADC 值添加到队列中
        adc_values_.push_back(power_value);
        if (adc_values_.size() > kBatteryAdcDataCount) {
            adc_values_.erase(adc_values_.begin());
        }
        uint32_t average_adc = 0;
        for (auto value : adc_values_) {
            average_adc += value;
        }
        average_adc /= adc_values_.size();

        // // 定义电池电压区间
        // const struct {
        //     uint16_t adc;
        //     uint8_t level;
        // } levels[] = {
        //     {1970, 0},
        //     {2062, 20},
        //     {2154, 40},
        //     {2246, 60},
        //     {2338, 80},
        //     {2430, 100}
        // };

        // 低于最低值时
        if (average_adc < POWER_VOLT_EMPTY) {
            battery_level_ = 0;
        }
        // 高于最高值时
        else if (average_adc >= POWER_VOLT_FULL) {
            battery_level_ = 100;
        } else {
            // // 线性插值计算中间值
            // for (int i = 0; i < 5; i++) {
            //     if (average_adc >= levels[i].adc && average_adc < levels[i + 1].adc) {
            //         float ratio = static_cast<float>(average_adc - levels[i].adc) / (levels[i + 1].adc - levels[i].adc);
            //         battery_level_ = levels[i].level + ratio * (levels[i + 1].level - levels[i].level);
            //         break;
            //     }
            // }

            battery_level_ = 100.0f * (power_value - POWER_VOLT_EMPTY) / (POWER_VOLT_FULL - POWER_VOLT_EMPTY);
        }

        // Check low battery status
        if (adc_values_.size() >= kBatteryAdcDataCount) {
            bool new_low_battery_status = battery_level_ <= kLowBatteryLevel;
            if (new_low_battery_status != is_low_battery_) {
                is_low_battery_ = new_low_battery_status;
                if (on_low_battery_status_changed_) {
                    on_low_battery_status_changed_(is_low_battery_);
                }
            }
        }

        ESP_LOGI("PowerManager", "ADC value: %f average: %ld level: %ld", power_value, average_adc, battery_level_);
    }

    static esp_err_t adc_calibration_init(adc_unit_t unit, adc_channel_t channel, adc_atten_t atten, adc_cali_handle_t *out_handle) {
        adc_cali_handle_t handle = NULL;
        esp_err_t ret = ESP_FAIL;
        bool calibrated = false;

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
        if (!calibrated) {
            adc_cali_curve_fitting_config_t cali_config = {
                .unit_id = unit,
                .atten = atten,
                .bitwidth = ADC_BITWIDTH_12,
            };
            ret = adc_cali_create_scheme_curve_fitting(&cali_config, &handle);
            if (ret == ESP_OK) {
                calibrated = true;
            }
        }
#endif

#if ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
        if (!calibrated) {
            ESP_LOGI(TAG, "calibration scheme version is %s", "Line Fitting");
            adc_cali_line_fitting_config_t cali_config = {
                .unit_id = unit,
                .atten = atten,
                .bitwidth = ADC_BITWIDTH_12,
            };
            ret = adc_cali_create_scheme_line_fitting(&cali_config, &handle);
            if (ret == ESP_OK) {
                calibrated = true;
            }
        }
#endif

        *out_handle = handle;

        return calibrated ? ESP_OK : ESP_FAIL;
    }

  public:
    PowerManager(gpio_num_t pin) : charging_pin_(pin) {
        // 初始化充电引脚
        gpio_config_t io_conf = {};
        io_conf.intr_type = GPIO_INTR_DISABLE;
        io_conf.mode = GPIO_MODE_INPUT;
        io_conf.pin_bit_mask = (1ULL << charging_pin_);
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        gpio_config(&io_conf);

        // 创建电池电量检查定时器
        esp_timer_create_args_t timer_args = {
            .callback = [](void *arg) {
                PowerManager *self = static_cast<PowerManager *>(arg);
                self->CheckBatteryStatus();
            },
            .arg = this,
            .dispatch_method = ESP_TIMER_TASK,
            .name = "battery_check_timer",
            .skip_unhandled_events = true,
        };
        ESP_ERROR_CHECK(esp_timer_create(&timer_args, &timer_handle_));
        ESP_ERROR_CHECK(esp_timer_start_periodic(timer_handle_, 1000000));

        // 初始化 ADC
        // https://docs.espressif.com/projects/esp-idf/zh_CN/v5.4/esp32s3/api-reference/peripherals/adc_oneshot.html#adc-oneshot-power-management
        adc_oneshot_unit_init_cfg_t init_config = {
            .unit_id = ADC_UNIT_1,
            // .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle_));

        adc_oneshot_chan_cfg_t chan_config = {
            .atten = ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_12,
        };
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc_handle_, POWER_ADC_CHANNEL, &chan_config));

        // 初始化ADC校准
        ESP_ERROR_CHECK(adc_calibration_init(ADC_UNIT_2, POWER_ADC_CHANNEL, ADC_ATTEN_DB_12, &power_adc_cali_handle));
    }

    ~PowerManager() {
        if (timer_handle_) {
            esp_timer_stop(timer_handle_);
            esp_timer_delete(timer_handle_);
        }
        if (adc_handle_) {
            adc_oneshot_del_unit(adc_handle_);
        }
    }

    bool IsCharging() {
        // 如果电量已经满了，则不再显示充电中
        if (battery_level_ == 100) {
            return false;
        }
        return is_charging_;
    }

    bool IsDischarging() {
        // 没有区分充电和放电，所以直接返回相反状态
        return !is_charging_;
    }

    uint8_t GetBatteryLevel() {
        return battery_level_;
    }

    void OnLowBatteryStatusChanged(std::function<void(bool)> callback) {
        on_low_battery_status_changed_ = callback;
    }

    void OnChargingStatusChanged(std::function<void(bool)> callback) {
        on_charging_status_changed_ = callback;
    }
};
