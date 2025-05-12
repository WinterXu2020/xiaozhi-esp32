#include "application.h"
#include "assets/lang_config.h"
#include "audio_codecs/no_audio_codec.h"
#include "button.h"
#include "config.h"
#include "display/lcd_display.h"
#include "iot/thing_manager.h"
#include "led/circular_strip.h"
#include "power_manager.h"
#include "power_save_timer.h"
#include "system_reset.h"
#include "wifi_board.h"

#include <driver/i2c_master.h>
#include <driver/rtc_io.h>
#include <driver/spi_common.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>
#include <esp_sleep.h>
#include <wifi_station.h>

#define TAG "ESP32S3ArmourSpeaker"

LV_FONT_DECLARE(font_puhui_16_4);
LV_FONT_DECLARE(font_awesome_16_4);

class ESPArmourSpeaker : public WifiBoard {
  private:
    Button menu_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    LcdDisplay *display_;
    PowerManager *power_manager_;
    // PowerSaveTimer *power_save_timer_;
    esp_lcd_panel_handle_t panel_ = nullptr;
    uint8_t brightness_ = 75;
    std::string BRIGHTNESS = "亮度 ";
    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        esp_lcd_panel_io_handle_t panel_io = nullptr;

        // 液晶屏控制IO初始化
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io));

        // 初始化液晶屏驱动芯片
        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;

        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io, &panel_config, &panel_));

        esp_lcd_panel_reset(panel_);

        esp_lcd_panel_init(panel_);
        esp_lcd_panel_invert_color(panel_, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel_, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);

        display_ = new SpiLcdDisplay(panel_io, panel_,
                                     DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY,
                                     {
                                         .text_font = &font_puhui_16_4,
                                         .icon_font = &font_awesome_16_4,
#if CONFIG_USE_WECHAT_MESSAGE_STYLE
                                         .emoji_font = font_emoji_32_init(),
#else
                                         .emoji_font = DISPLAY_HEIGHT >= 240 ? font_emoji_64_init() : font_emoji_32_init(),
#endif
                                     });
    }

    // 各类按钮功能实现
    // +-按钮调整音量。长按 +- 按钮，调整屏幕亮度，
    void InitializeButtons() {

        // 正常状态单击则打断说话
        menu_button_.OnClick([this]() {
            std::string wake_word = "你好小智";
            Application::GetInstance().WakeWordInvoke(wake_word);
        });

        // 处于开启状态，并且没联网则重置wifi
        menu_button_.OnLongPress([this]() {
            auto &app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting && !WifiStation::GetInstance().IsConnected()) {
                ResetWifiConfiguration();
            }
            app.ToggleChatState();
        });

        volume_up_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_up_button_.OnLongPress([this]() {
            brightness_ = brightness_ + 10;
            if (brightness_ > 100) {
                brightness_ = 100;
            }
            GetBacklight()->SetBrightness(brightness_);
            GetDisplay()->ShowNotification(BRIGHTNESS + std::to_string(brightness_));
        });

        volume_down_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
                codec->SetOutputVolume(volume);
                GetDisplay()->ShowNotification(Lang::Strings::MUTED);
            } else {
                codec->SetOutputVolume(volume);
                GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
            }
        });

        volume_down_button_.OnLongPress([this]() {
            brightness_ = brightness_ - 10;
            if (brightness_ < 10) {
                brightness_ = 10;
            }
            GetBacklight()->SetBrightness(brightness_);
            GetDisplay()->ShowNotification(BRIGHTNESS + std::to_string(brightness_));
        });
    }

    // 物联网初始化，添加对 AI 可见设备
    void InitializeIot() {
        auto &thing_manager = iot::ThingManager::GetInstance();
        thing_manager.AddThing(iot::CreateThing("Speaker"));
        thing_manager.AddThing(iot::CreateThing("Screen"));
        thing_manager.AddThing(iot::CreateThing("Lamp"));
        
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            thing_manager.AddThing(iot::CreateThing("Backlight"));
        }
    }

    // 电池管理
    void InitializePowerManager() {
        power_manager_ = new PowerManager(BATTERY_ADC_GPIO);
        // power_manager_->OnChargingStatusChanged([this](bool is_charging) {
        //     if (is_charging) {
        //         power_save_timer_->SetEnabled(false);
        //     } else {
        //         power_save_timer_->SetEnabled(true);
        //     }
        // });
    }

    // 低功耗管理
    // void InitializePowerSaveTimer() {
    //     rtc_gpio_init(ESP_RTC_GPIO);
    //     rtc_gpio_set_direction(ESP_RTC_GPIO, RTC_GPIO_MODE_OUTPUT_ONLY);
    //     rtc_gpio_set_level(ESP_RTC_GPIO, 1);

    //     power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
    //     power_save_timer_->OnEnterSleepMode([this]() {
    //         ESP_LOGI(TAG, "Enabling sleep mode");
    //         display_->SetChatMessage("system", "");
    //         display_->SetEmotion("sleepy");
    //         GetBacklight()->SetBrightness(1);
    //     });
    //     power_save_timer_->OnExitSleepMode([this]() {
    //         display_->SetChatMessage("system", "");
    //         display_->SetEmotion("neutral");
    //         GetBacklight()->RestoreBrightness();
    //     });
    //     power_save_timer_->OnShutdownRequest([this]() {
    //         ESP_LOGI(TAG, "Shutting down");
    //         rtc_gpio_set_level(ESP_RTC_GPIO, 0);
    //         // 启用保持功能，确保睡眠期间电平不变
    //         rtc_gpio_hold_en(ESP_RTC_GPIO);
    //         esp_lcd_panel_disp_on_off(panel_, false); //关闭显示
    //         esp_deep_sleep_start();
    //     });
    //     power_save_timer_->SetEnabled(true);
    // }

  public:
    ESPArmourSpeaker() : menu_button_(MENU_BUTTON_GPIO),
                         volume_up_button_(VOLUME_UP_BUTTON_GPIO),
                         volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializeSpi();
        InitializeLcdDisplay();
        InitializeButtons();
        InitializeIot();
        InitializePowerManager();
        // InitializePowerSaveTimer();

        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            GetBacklight()->RestoreBrightness();
        }
    }

    virtual Led *GetLed() override {
        static CircularStrip led(BUILTIN_LED_GPIO, BUILTIN_LED_NUM);

        return &led;
    }

    virtual AudioCodec *GetAudioCodec() override {
        static NoAudioCodecSimplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
                                               AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);

        return &audio_codec;
    }

    virtual Display *GetDisplay() override {
        return display_;
    }

    virtual Backlight *GetBacklight() override {
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
            return &backlight;
        }
        return nullptr;
    }

    virtual bool GetBatteryLevel(int &level, bool &charging, bool &discharging) override {
        static bool last_discharging = false;
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        if (discharging != last_discharging) {
            // power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }
        level = power_manager_->GetBatteryLevel();
        return true;
    }


    // virtual void SetPowerSaveMode(bool enabled) override {
    //     if (!enabled) {
    //         power_save_timer_->WakeUp();
    //     }
    //     WifiBoard::SetPowerSaveMode(enabled);
    // }
};

DECLARE_BOARD(ESPArmourSpeaker);
