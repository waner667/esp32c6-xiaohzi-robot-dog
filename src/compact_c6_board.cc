#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/oled_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "lamp_controller.h"
#include "dog_leg_controller.h"
#include "led/single_led.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <array>

#ifdef SH1106
#include <esp_lcd_panel_sh1106.h>
#endif

#define TAG "CompactC6Board"

namespace {

// 延迟绑定的 Display 代理。
//
// 2026-10-14 的教训：SSD1306 的 charge pump 上电要好几秒才应答，早期版本在启动
// 阶段探测不到屏，就把 display_ 设成 NoDisplay，等后台任务几秒后探到再换成真的
// OledDisplay。但 Application::Initialize() 早就把 board.GetDisplay() 的返回值
// **缓存成局部指针**并跑过 SetupUI()，换了底层对象它也不知道 —— 症状是屏闪一下
// （驱动 init 刷了一帧 RAM）然后一直黑。
//
// 这里让 display_ 从一开始就是**同一个对象**：所有 UI 调用先落到代理上，代理记住
// 最近一次状态；真屏就绪时 Attach() 把真屏塞进去，按顺序补放SetupUI + 状态 +
// 对话气泡。这样无论屏什么时候醒，界面都会正确重建，且指针永不变。
class DeferredDisplay : public Display {
public:
    DeferredDisplay(int w, int h) {
        width_ = w;
        height_ = h;
    }

    // 接上真屏并补放之前缓存的 UI 状态。可重复调用（幂等）。
    void Attach(Display* real) {
        if (real == nullptr || real_ == real) {
            return;
        }
        real_ = real;
        if (setup_ui_called_) {
            real_->SetupUI();
        }
        if (!last_status_.empty()) {
            real_->SetStatus(last_status_.c_str());
        }
        if (!last_emotion_.empty()) {
            real_->SetEmotion(last_emotion_.c_str());
        }
        if (!last_chat_role_.empty() || !last_chat_content_.empty()) {
            real_->SetChatMessage(last_chat_role_.c_str(), last_chat_content_.c_str());
        }
        ESP_LOGI("DeferredDisplay", "attached real display, replayed UI state");
    }

    void SetStatus(const char* status) override {
        if (status) {
            last_status_ = status;
        }
        if (real_) {
            real_->SetStatus(status);
        }
    }

    void SetEmotion(const char* emotion) override {
        if (emotion) {
            last_emotion_ = emotion;
        }
        if (real_) {
            real_->SetEmotion(emotion);
        }
    }

    void SetChatMessage(const char* role, const char* content) override {
        if (role) {
            last_chat_role_ = role;
        }
        if (content) {
            last_chat_content_ = content;
        }
        if (real_) {
            real_->SetChatMessage(role, content);
        }
    }

    void SetupUI() override {
        setup_ui_called_ = true;
        if (real_) {
            real_->SetupUI();
        }
    }

    void ShowNotification(const char* notification, int duration_ms) override {
        if (real_) {
            real_->ShowNotification(notification, duration_ms);
        }
    }

    void ShowNotification(const std::string& notification, int duration_ms) override {
        if (real_) {
            real_->ShowNotification(notification, duration_ms);
        }
    }

    void SetTheme(Theme* theme) override {
        if (real_) {
            real_->SetTheme(theme);
        }
    }

    void UpdateStatusBar(bool update_all) override {
        if (real_) {
            real_->UpdateStatusBar(update_all);
        }
    }

    void SetPowerSaveMode(bool on) override {
        if (real_) {
            real_->SetPowerSaveMode(on);
        }
    }

    bool IsMonochrome() const override {
        return real_ ? real_->IsMonochrome() : true;
    }

private:
    Display* real_ = nullptr;
    std::string last_status_;
    std::string last_emotion_;
    std::string last_chat_role_;
    std::string last_chat_content_;

    virtual bool Lock(int timeout_ms = 0) override {
        (void)timeout_ms;
        return true;
    }
    virtual void Unlock() override {}
};

}  // namespace

class CompactC6Board : public WifiBoard {
private:
    i2c_master_bus_handle_t display_i2c_bus_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;
    Display* display_ = nullptr;
    DeferredDisplay* deferred_display_ = nullptr;  // display_ 实际指向它，屏就绪后 Attach
    Button boot_button_;
    Button touch_button_;
    Button volume_up_button_;
    Button volume_down_button_;

    void InitializeDisplayI2c() {
        i2c_master_bus_config_t bus_config = {
            .i2c_port = (i2c_port_t)0,
            .sda_io_num = DISPLAY_SDA_PIN,
            .scl_io_num = DISPLAY_SCL_PIN,
            .clk_source = I2C_CLK_SRC_DEFAULT,
            .glitch_ignore_cnt = 15,
            .intr_priority = 0,
            .trans_queue_depth = 0,
            .flags = {
                .enable_internal_pullup = 1,
            },
        };
        ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &display_i2c_bus_));
    }

    // 探测某个 7 位地址上是否有从机应答。
    // 两阶段：先发 SSD1306 真实初始化序列（多字节连续传输，与驱动后续行为一致），
    // 失败再退回单字节 0x00 宽松探测 —— 弱上拉下单字节更容易过，
    // 命中时会打印 WEAK 提示，说明后续驱动初始化可能仍会失败。
    bool ProbeI2cAddress(uint8_t addr, uint32_t scl_hz) {
        i2c_master_dev_handle_t dev = nullptr;
        i2c_device_config_t dev_config = {};
        dev_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        dev_config.device_address = addr;
        dev_config.scl_speed_hz = scl_hz;
        if (i2c_master_bus_add_device(display_i2c_bus_, &dev_config, &dev) != ESP_OK) {
            return false;
        }

        // SSD1306 真实初始化序列（与 esp_lcd_panel_ssd1306 内部使用的一致）
        static const uint8_t init_seq[] = {
            0x00,                          // control byte: command stream
            0xAE,                          // display off
            0xD5, 0x80,// clock divide
            0xA8, 0x1F,                    // multiplex = 32 (128x32)
            0xD3, 0x00,                    // display offset
            0x40,                          // display start line 0
            0x8D, 0x14,                    // charge pump enable
            0x20, 0x02,                    // page addressing mode
            0xA1,                          // segment remap
            0xC8,                          // COM output scan direction
            0xDA, 0x02,                    // COM pins config
            0x81, 0xCF,                    // contrast
            0xD9, 0xF1,                    // pre-charge
            0xDB, 0x40,                    // VCOMH deselect
            0xA4,                          // resume from RAM
            0xA6,                          // normal (not inverted)
            0xAF,                          // display on
        };

        bool pass = i2c_master_transmit(dev, init_seq, sizeof(init_seq), 200) == ESP_OK;
        if (!pass) {
            uint8_t single = 0x00;
            pass = i2c_master_transmit(dev, &single, 1, 200) == ESP_OK;
            if (pass) {
                ESP_LOGW(TAG, "    (weak: addr=0x%02X only ACK'd single-byte, link quality poor)",
                         addr);
            }
        }

        i2c_master_bus_rm_device(dev);
        return pass;
    }

    // 扫一遍所有地址/速率组合，命中就返回 true 并输出地址与速率。
    bool DetectOled(uint8_t* out_addr, uint32_t* out_hz, int retries_per_combo) {
        const uint8_t  addrs[]  = {0x3C, 0x3D};
        const uint32_t speeds[] = {400 * 1000, 100 * 1000};
        for (int s = 0; s < 2; s++) {
            for (int i = 0; i < 2; i++) {
                int hits = 0;
                for (int r = 0; r < retries_per_combo; r++) {
                    if (ProbeI2cAddress(addrs[i], speeds[s])) {
                        hits++;
                        if (out_addr) *out_addr = addrs[i];
                        if (out_hz)   *out_hz   = speeds[s];
                    }
                    vTaskDelay(pdMS_TO_TICKS(15));
                }
                ESP_LOGW(TAG, "  probe %s: addr=0x%02X @ %luHz (%d/%d)",
                         hits ? "HIT " : "miss", addrs[i], (unsigned long)speeds[s],
                         hits, retries_per_combo);
                if (hits) return true;
                vTaskDelay(pdMS_TO_TICKS(20));
            }
        }
        return false;
    }

    // 用已知地址/速率把驱动装起来并点亮。返回是否成功。
    bool InstallPanelAndShow(uint8_t addr, uint32_t hz) {
        if (panel_io_ != nullptr) {
            return true;  // 已装好
        }
        esp_lcd_panel_io_i2c_config_t io_config = {
            .dev_addr = addr,
            .scl_speed_hz = hz,
            .control_phase_bytes = 1,
            .dc_bit_offset = 6,
            .lcd_cmd_bits = 8,
            .lcd_param_bits = 8,
            .on_color_trans_done = nullptr,
            .user_ctx = nullptr,
            .flags = {
                .dc_low_on_data = 0,
                .disable_control_phase = 0,
            },
            .transaction_timeout_ms = 1000,
        };
        if (esp_lcd_new_panel_io_i2c(display_i2c_bus_, &io_config, &panel_io_) != ESP_OK) {
            ESP_LOGE(TAG, "esp_lcd_new_panel_io_i2c failed");
            return false;
        }

        ESP_LOGI(TAG, "Install SSD1306 driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = GPIO_NUM_NC;
        panel_config.bits_per_pixel = 1;

        esp_lcd_panel_ssd1306_config_t ssd1306_config = {
            .height = static_cast<uint8_t>(DISPLAY_HEIGHT),
        };
        panel_config.vendor_config = &ssd1306_config;

#ifdef SH1106
        esp_err_t drv = esp_lcd_new_panel_sh1106(panel_io_, &panel_config, &panel_);
#else
        esp_err_t drv = esp_lcd_new_panel_ssd1306(panel_io_, &panel_config, &panel_);
#endif
        if (drv != ESP_OK) {
            ESP_LOGE(TAG, "esp_lcd_new_panel_ssd1306 failed: %s", esp_err_to_name(drv));
            return false;
        }
        ESP_LOGI(TAG, "SSD1306 driver installed");

        // 探测阶段已经把屏置为 display on，这里先关掉，
        // 让 esp_lcd_panel_reset + panel_init 从干净状态重新走完整初始化，
        // 否则驱动内部状态与屏实际状态不一致会导致 init 失败。
        //
        // 2026-10-14 修复：这里原本传 NULL + len=0，IDF 6.1 的 i2c master 内部有
        // ESP_ASSERT 拦截空数据指针，直接 abort()（MCAUSE 0x2）。虽然现在已经改成
        // 用单字节缓冲，这里干脆直接跳过这步，交给下面的 reset+init。
        vTaskDelay(pdMS_TO_TICKS(50));

        esp_err_t rst_err = esp_lcd_panel_reset(panel_);
        if (rst_err != ESP_OK) {
            ESP_LOGE(TAG, "esp_lcd_panel_reset failed: %s", esp_err_to_name(rst_err));
            return false;
        }
        if (esp_lcd_panel_init(panel_) != ESP_OK) {
            ESP_LOGE(TAG, "Failed to initialize display");
            return false;
        }
        esp_err_t inv_err = esp_lcd_panel_invert_color(panel_, false);
        if (inv_err != ESP_OK) {
            ESP_LOGW(TAG, "invert_color failed (ignored): %s", esp_err_to_name(inv_err));
        }

        ESP_LOGI(TAG, "Turning display on");
        if (esp_lcd_panel_disp_on_off(panel_, true) != ESP_OK) {
            ESP_LOGE(TAG, "esp_lcd_panel_disp_on_off failed");
            return false;
        }

        // 注意：**不要**把 display_ 换成这个 OledDisplay。
        // Application::Initialize() 早就缓存了 GetDisplay() 的返回值，换指针它收不到。
        // 正确做法是塞进启动时就建好的 DeferredDisplay 代理，由代理补放 UI 状态。
        Display* real = new OledDisplay(panel_io_, panel_, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                         DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
        if (deferred_display_ != nullptr) {
            deferred_display_->Attach(real);
        } else {
            display_ = real;
        }
        ESP_LOGI(TAG, "OLED IS LIVE");
        return true;
    }

    // 后台探测任务：SSD1306 的 charge pump 上电要好几秒才应答（实测启动 150ms 和
    // 2.5s 都探不到，运行几秒后 0x3C 才稳定应答），所以这里后台一直重探，
    // 命中就装驱动并 Attach 到代理上。不再手动补画 UI —— 代理会重放。
    static void OledWatchTask(void* arg) {
        CompactC6Board* self = static_cast<CompactC6Board*>(arg);
        while (true) {
            uint8_t  addr = 0;
            uint32_t hz   = 0;
            if (self->DetectOled(&addr, &hz, 1)) {
                ESP_LOGW(TAG, "*** OLED WATCH: HIT addr=0x%02X @ %luHz, installing driver ***",
                         addr, (unsigned long)hz);
                if (self->InstallPanelAndShow(addr, hz)) {
                    vTaskDelete(nullptr);
                    return;
                }
                // 装驱动失败就把 io 句柄清掉，下轮重新来
                self->panel_io_ = nullptr;
                self->panel_ = nullptr;
            } else {
                ESP_LOGW(TAG, "OLED WATCH: still no ACK on 0x3C/0x3D @ 400k/100k");
            }
            vTaskDelay(pdMS_TO_TICKS(2000));
        }
    }

    void InitializeSsd1306Display() {
        // 自动探测（2026-10-14）。
        //
        // 关键事实：屏的 charge pump 上电很慢，实测启动 150ms 和 2.5s 都探不到，
        // 运行几秒后 0x3C @ 400k/100k 才一直应答。所以这里**不在启动阶段死等**
        // （那会白白拖慢开机），而是：启动时快速探一次，探不到就交给后台任务，
        // 界面靠 DeferredDisplay 代理缓存 + 重放，两条路径都能正确显示。
        //
        // 速率只用 400k/100k：50kHz 在 IDF 6.1 的 i2c master 上会让设备卡死
        //（日志断在探测中途，无任何后续输出，实测两次）。
        //
        // 注意：不要试图用 i2c_del_master_bus + i2c_new_master_bus 来切换引脚方向。
        // i2c_master_bus_destroy 内部 free(handle) 之后只置空自己的局部副本，
        // 调用方的指针变成悬垂指针，再赋值就是 Load access fault（已实测崩溃）。
        uint8_t  detected_addr = 0;
        uint32_t detected_hz   = 0;

        // display_ 一次创建、终身不变。真屏就绪时 InstallPanelAndShow 会 Attach 进去。
        deferred_display_ = new DeferredDisplay(DISPLAY_WIDTH, DISPLAY_HEIGHT);
        display_ = deferred_display_;

        ESP_LOGW(TAG, "=== OLED detect start (SDA=%d SCL=%d) ===",
                 DISPLAY_SDA_PIN, DISPLAY_SCL_PIN);

        // 短暂等一下让 charge pump 起个步，然后快速探一次（每组合只试 2 次）。
        // 探不到不阻塞开机，直接交给后台任务。
        vTaskDelay(pdMS_TO_TICKS(500));

        if (DetectOled(&detected_addr, &detected_hz, 2) &&
            InstallPanelAndShow(detected_addr, detected_hz)) {
            return;  // 启动即点亮
        }

        ESP_LOGW(TAG, "OLED not ready yet, starting background retry task");
        xTaskCreate(OledWatchTask, "oled_watch", 4096, this, 3, nullptr);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
        touch_button_.OnPressDown([this]() {
            Application::GetInstance().StartListening();
        });
        touch_button_.OnPressUp([this]() {
            Application::GetInstance().StopListening();
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
            GetAudioCodec()->SetOutputVolume(100);
            GetDisplay()->ShowNotification(Lang::Strings::MAX_VOLUME);
        });

        volume_down_button_.OnClick([this]() {
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_down_button_.OnLongPress([this]() {
            GetAudioCodec()->SetOutputVolume(0);
            GetDisplay()->ShowNotification(Lang::Strings::MUTED);
        });
    }

    // 物联网初始化，逐步迁移到 MCP 协议
    void InitializeTools() {
        static LampController lamp(LAMP_GPIO);

        // 四足桌宠舵机（SG90 × 4）。
        //
        // 供电要求（重要）：舵机必须用**外部独立 5V/≥3A 电源**，不能从板子 5V 取电，
        // 且外部电源的 GND 必须与 ESP32 的 GND 共地 —— 否则 PWM 脉冲没有参考电平，
        // 舵机会乱抖甚至持续堵转发热。电源入口建议并联 1000µF 电解电容。
        static DogLegController dog({
            SERVO_GPIO_RF,  // 舵机1 右前
            SERVO_GPIO_RB,  // 舵机2 右后
            SERVO_GPIO_LF,  // 舵机3 左前
            SERVO_GPIO_LB,  // 舵机4 左后
        });
        (void)dog;
    }

public:
    CompactC6Board() :
        boot_button_(BOOT_BUTTON_GPIO),
        touch_button_(TOUCH_BUTTON_GPIO),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO) {
        InitializeDisplayI2c();
        InitializeSsd1306Display();
        InitializeButtons();
        InitializeTools();
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
        // ESP32-C6 只有一个 I2S 控制器，只能使用 Duplex 共享时钟模式
        static NoAudioCodecDuplex audio_codec(AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
};

DECLARE_BOARD(CompactC6Board);
