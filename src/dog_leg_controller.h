#ifndef __DOG_LEG_CONTROLLER_H__
#define __DOG_LEG_CONTROLLER_H__

// ===== 调试开关：开机自动循环摆腿 =====
// 打开后，固件启动几秒会自动开始循环切换明显角度的动作，
// 不依赖语音/MCP，用来现场确认舵机硬件（供电/共地/信号线）是否通路。
// 硬件已验证通过（2026-10-16），正式使用改为 0 —— 恢复"只在对话指令下才动"。
#ifndef DOG_AUTO_CYCLE
#define DOG_AUTO_CYCLE 0
#endif

#include <driver/ledc.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "mcp_server.h"

#define TAG_DOG "DogLegCtrl"

// 四足桌宠：4 个 SG90 舵机 + 髋部摆动
//
// 机械约定（左右镜像安装）：
//   左腿(舵机3/4) 从 0° 往上加，右腿(舵机1/2) 从 180° 往下减；90° = 中位/站立基准。
//   因此对镜像的一对腿，右腿角度 = 180 - 左腿角度。
//
// 安全设计（这几条都是烧硬件换来的，不要删）：
//   1) 软件硬夹限位 kMinAngle/kMaxAngle = 10°/170°。SG90 顶到 0°/180° 会持续堵转、
//      发热，严重时烧齿轮。嘉立创原版用的就是 0/180，这里留了 10° 余量。
//   2) 上电不直接给目标角度，而是先回 90° 中位并等 300ms，让四个舵机同时到位，
//      避免上电瞬间四个同时猛冲到限位。
//   3) 动作之间用小步长插值（默认 5°/步、20ms/步）而不是一步到位 —— 保护齿轮，
//      观感也更像真狗。
//   4) MCP 只暴露语义动作名，绝不让 AI 传裸角度。传角度等于让语言模型直接控制
//      机械限位，是会烧硬件的设计。
class DogLegController {
public:
    static constexpr int kServoCount = 4;

    // 一个姿态就是四个舵机的目标角度（顺序：舵机1(RF), 舵机2(RB), 舵机3(LF), 舵机4(LB)）
    struct Pose {
        int angle[kServoCount];
    };

    // 姿态库（左右镜像：左腿3/4 从0°加，右腿1/2 从180°减）
    static const Pose kPoseStand;
    static const Pose kPoseSitdown;
    static const Pose kPoseLie;
    static const Pose kPoseSleep;
    static const Pose kPoseBow;
    static const Pose kPoseAlertL;
    static const Pose kPoseAlertR;

private:
    // SG90 180° 版：500µs = 0°，2500µs = 180°，周期 20ms(50Hz)
    static constexpr uint32_t kServoPeriodUs   = 20000;
    static constexpr uint32_t kPulseMinUs      = 500;
    static constexpr uint32_t kPulseMaxUs      = 2500;
    static constexpr int      kMidAngle        = 90;       // 站立基准（中位）
    static constexpr int      kMinAngle        = 10;       // 硬限位下界
    static constexpr int      kMaxAngle        = 170;      // 硬限位上界

    // LEDC 分辨率位数。duty 是"计数值"，不是微秒 —— 这是本项目踩过的最大坑。
    // ledc_set_duty 的 duty 取值范围是 [0, 2^duty_resolution]，
    // 而 2^20 = 1048576，所以真正能用的 pulse 只有 duty/1048576*20ms 这么宽。
    static constexpr int kDutyResBits = 20;

    // 插值参数
    static constexpr int kStepDegrees   = 5;   // 每步转多少度
    static constexpr int kStepDelayMs   = 20;  // 每步间隔

    // 上电回中位后的等待时间（等舵机实际到位）
    static constexpr int kPowerOnSettleMs = 300;

    gpio_num_t pins_[kServoCount];
    ledc_channel_t channels_[kServoCount];

    // 当前实际角度（用于从当前姿态平滑过渡）
    int current_[kServoCount];

    // 每条腿的安装偏置角（度）。
    //
    // 用途：舵机臂的机械安装角度不可能四腿完全一致（打印件孔位公差 + 装配误差），
    // 用它把"软件角度"对齐到"实际机械朝向"，不用为了对角度去拆装舵机臂。
    // 在 ClampAngle 之后叠加，再做一次安全夹紧。
    // 可通过 MCP 工具 self.dog.calibrate 在线调整（调试期），调好后写回本数组即可固化。
    int offset_[kServoCount] = {0, 0, 0, 0};

    bool ready_ = false;

    // 硬件限位夹紧：任何角度写入前都必须过这一关
    static int ClampAngle(int deg) {
        return std::max(kMinAngle, std::min(kMaxAngle, deg));
    }

    // 逻辑角度 -> 实际下发给硬件的角度（先夹紧，再叠安装偏置）
    int ApplyOffset(int i, int deg) const {
        return ClampAngle(deg + offset_[i]);
    }

    // 角度 -> LEDC duty。**duty 的单位是定时器计数值，不是微秒！**
    //
    // 2026-10-16 踩过的坑（害得整机舵机完全不动，排查了很久硬件）：
    //   原来这里直接 return 微秒数(500~2500)。但 ledc_set_duty 要求的是
    //   [0, 2^duty_resolution] 范围内的计数值。20 位分辨率下 2^20 = 1048576，
    //   传 1500 得到的实际脉冲只有 1500/1048576*20000 = 28.6µs，
    //   远低于 SG90 的 500µs 识别阈值 → 舵机有电（锁死有阻力）但完全不动。
    //
    // 正确换算：duty = pulse_width_us * 2^res / period_us
    //   500µs  -> 26214     (0°)
    //   1500µs -> 78643     (90° 中位)
    //   2500µs -> 131072    (180°)
    static uint32_t AngleToDuty(float deg) {
        if (deg < 0.0f)   deg = 0.0f;
        if (deg > 180.0f) deg = 180.0f;
        uint32_t span = kPulseMaxUs - kPulseMinUs;
        uint32_t pulse_us = (uint32_t)(kPulseMinUs + span * deg / 180.0f);
        uint32_t full_scale = (1UL << kDutyResBits);
        uint32_t duty = (uint32_t)((uint64_t)pulse_us * full_scale / kServoPeriodUs);
        return duty;
    }

    // 写入一路舵机。deg 是**逻辑角度**（姿态库里的值），实际下发给硬件的是
    // deg + offset_[i]。current_ 记录逻辑角度，这样 GlideOne 插值从逻辑角度算，
    // 不会被偏置干扰。
    void WriteOne(int i, int deg) {
        deg = ClampAngle(deg);
        current_[i] = deg;
        uint32_t duty = AngleToDuty((float)ApplyOffset(i, deg));
        ledc_set_duty(LEDC_LOW_SPEED_MODE, channels_[i], duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, channels_[i]);
    }

    // 把某个舵机平滑走到目标角度（分步插值）
    void GlideOne(int i, int target, int step_deg = kStepDegrees, int step_ms = kStepDelayMs) {
        target = ClampAngle(target);
        int start = current_[i];
        int delta = target - start;
        if (delta == 0) {
            return;
        }
        int steps = (std::abs(delta) + step_deg - 1) / step_deg;
        if (steps <= 0) {
            return;
        }
        for (int s = 1; s <= steps; s++) {
            int deg = start + delta * s / steps;
            WriteOne(i, deg);
            vTaskDelay(pdMS_TO_TICKS(step_ms));
        }
    }

    // 姿态序列的一步：p[0..3] 是四个舵机目标角度，step_ms 可为 0（不等待）
    void PoseStep(const int* p, int step_ms) {
        for (int i = 0; i < kServoCount; i++) {
            int target = ClampAngle(p[i]);
            // 同步写入，不做逐步插值（用于步态里的"整组瞬间到位"）
            WriteOne(i, target);
        }
        if (step_ms > 0) {
            vTaskDelay(pdMS_TO_TICKS(step_ms));
        }
    }

    // 平滑走到一个完整姿态（四腿同时插值）
    void GlidePose(const Pose& pose, int step_ms = kStepDelayMs) {
        for (int i = 0; i < kServoCount; i++) {
            GlideOne(i, pose.angle[i], kStepDegrees, step_ms);
        }
    }

    // ---- 动作实现 ----

    void Stand() {
        GlidePose(kPoseStand);
    }

    void SitDown() {
        GlidePose(kPoseSitdown);
        vTaskDelay(pdMS_TO_TICKS(600));
        GlidePose(kPoseStand);
    }

    void Lie() {
        GlidePose(kPoseLie);
        vTaskDelay(pdMS_TO_TICKS(600));
    }

    void Sleep() {
        GlidePose(kPoseSleep);
        vTaskDelay(pdMS_TO_TICKS(600));
    }

    void Wake() {
        GlidePose(kPoseStand);
    }

    void Bow() {
        GlidePose(kPoseBow);
        vTaskDelay(pdMS_TO_TICKS(800));
        GlidePose(kPoseStand);
    }

    void Alert() {
        GlidePose(kPoseAlertL);
        vTaskDelay(pdMS_TO_TICKS(250));
        GlidePose(kPoseAlertR);
        vTaskDelay(pdMS_TO_TICKS(250));
        GlidePose(kPoseStand);
    }

    // 摇身 / 摆尾：在站立基础上左右小幅交替
    void Wag(int repeat = 3) {
        if (repeat < 1)  repeat = 1;
        if (repeat > 8)  repeat = 8;
        for (int r = 0; r < repeat; r++) {
            const Pose& a = kPoseAlertR;
            const Pose& b = kPoseAlertL;
            GlidePose(a);
            vTaskDelay(pdMS_TO_TICKS(180));
            GlidePose(b);
            vTaskDelay(pdMS_TO_TICKS(180));
        }
        GlidePose(kPoseStand);
    }

    // 举左前腿（打招呼）
    void RaiseLeft(int repeat = 2) {
        for (int r = 0; r < repeat; r++) {
            Pose lift = kPoseStand;
            lift.angle[2] = 40;  // 左前腿抬
            GlidePose(lift);
            vTaskDelay(pdMS_TO_TICKS(350));
        }
        GlidePose(kPoseStand);
    }

    // 举右前腿
    void RaiseRight(int repeat = 2) {
        for (int r = 0; r < repeat; r++) {
            Pose lift = kPoseStand;
            lift.angle[0] = 140;  // 右前腿抬
            GlidePose(lift);
            vTaskDelay(pdMS_TO_TICKS(350));
        }
        GlidePose(kPoseStand);
    }

    // 前进：对角步态。2/3 先迈 → 1/4 后蹬 → 交换 → 归位。
    void Front(int repeat = 2) {
        if (repeat < 1)  repeat = 1;
        if (repeat > 5)  repeat = 5;
        for (int r = 0; r < repeat; r++) {
            Pose p1 = {{90, 140, 40, 90}};   // 左前+右后前迈
            Pose p2 = {{90,  90, 90, 90}};   // 归位
            Pose p3 = {{40,  90, 90, 140}};  // 右前+左后前迈
            Pose p4 = {{90,  90, 90, 90}};   // 归位
            GlidePose(p1); vTaskDelay(pdMS_TO_TICKS(120));
            GlidePose(p2); vTaskDelay(pdMS_TO_TICKS(120));
            GlidePose(p3); vTaskDelay(pdMS_TO_TICKS(120));
            GlidePose(p4); vTaskDelay(pdMS_TO_TICKS(120));
        }
    }

    // 后退：front 反向
    void Back(int repeat = 2) {
        if (repeat < 1)  repeat = 1;
        if (repeat > 5)  repeat = 5;
        for (int r = 0; r < repeat; r++) {
            Pose p3 = {{40,  90, 90, 140}};
            Pose p2 = {{90,  90, 90, 90}};
            Pose p1 = {{90, 140, 40, 90}};
            Pose p4 = {{90,  90, 90, 90}};
            GlidePose(p3); vTaskDelay(pdMS_TO_TICKS(120));
            GlidePose(p2); vTaskDelay(pdMS_TO_TICKS(120));
            GlidePose(p1); vTaskDelay(pdMS_TO_TICKS(120));
            GlidePose(p4); vTaskDelay(pdMS_TO_TICKS(120));
        }
    }

    // 左转：循环 3 次，身体左偏摆
    void TurnLeft(int repeat = 3) {
        if (repeat < 1)  repeat = 1;
        if (repeat > 6)  repeat = 6;
        for (int r = 0; r < repeat; r++) {
            Pose a = {{80, 80, 120, 120}};
            Pose b = {{40, 40, 140, 140}};
            Pose c = {{60, 60, 100, 100}};
            GlidePose(a); vTaskDelay(pdMS_TO_TICKS(150));
            GlidePose(b); vTaskDelay(pdMS_TO_TICKS(150));
            GlidePose(c); vTaskDelay(pdMS_TO_TICKS(150));
        }
        GlidePose(kPoseStand);
    }

    // 右转：left 反向
    void TurnRight(int repeat = 3) {
        if (repeat < 1)  repeat = 1;
        if (repeat > 6)  repeat = 6;
        for (int r = 0; r < repeat; r++) {
            Pose c = {{120, 120, 80, 80}};
            Pose b = {{140, 140, 40, 40}};
            Pose a = {{100, 100, 60, 60}};
            GlidePose(c); vTaskDelay(pdMS_TO_TICKS(150));
            GlidePose(b); vTaskDelay(pdMS_TO_TICKS(150));
            GlidePose(a); vTaskDelay(pdMS_TO_TICKS(150));
        }
        GlidePose(kPoseStand);
    }

    void RegisterMcpTools();

    // 开机自动循环摆腿（调试用，由 DOG_AUTO_CYCLE 控制）
    void StartAutoCycle();
    static void AutoCycleTaskTrampoline(void* arg);
    void AutoCycleTask();

public:
    // pins 顺序：{舵机1(RF), 舵机2(RB), 舵机3(LF), 舵机4(LB)}
    explicit DogLegController(const std::array<gpio_num_t, kServoCount>& pins);

    bool ready() const { return ready_; }

    // 状态字符串（给 MCP get_state 用）
    std::string StateJson() const;

    // 语义动作分发。MCP 只走这条路。
    bool DoAction(const std::string& action, int repeat);

    // ===== 安装标定（舵机臂装歪时用，不用重烧固件就能在线对齐）=====

    // 立刻把第 i 条腿写到指定逻辑角度（用于试角度、听声音判断是否堵转）
    bool ProbeLeg(int i, int deg);

    // 给第 i 条腿的安装偏置加 delta 度，并把当前逻辑角度重新下发一次
    // 返回新的偏置值。用于"这条腿高了 8 度 → trim +8"。
    int  AdjustOffset(int i, int delta);

    // 读/写某条腿的偏置
    int  GetOffset(int i) const { return offset_[i]; }
    bool SetOffset(int i, int deg);

    // 静态的腿名 <-> 序号映射，方便 MCP 接受 "front_left" / "左前腿" 这类自然表达
    static int LegIndexFromName(const std::string& name);   // -1 = 解析失败
    static const char* LegName(int i);

    // 动作清单（写进 MCP 描述，让 AI 知道能做什么）
    static std::vector<std::string> SupportedActions();
};



// 姿态库定义。左右镜像安装：左腿(3/4) 从 0° 往上加，右腿(1/2) 从 180° 往下减，
// 90° = 中位/站立基准。对镜像的一对腿，右 = 180 - 左。
// header-only 实现，所以用 inline 避免多 TU 重复定义。
inline const DogLegController::Pose DogLegController::kPoseStand    = {{ 90,  90,  90,  90}};
inline const DogLegController::Pose DogLegController::kPoseSitdown = {{140,  90,  40,  90}};
inline const DogLegController::Pose DogLegController::kPoseLie     = {{170,  10,  10, 170}};
inline const DogLegController::Pose DogLegController::kPoseSleep   = {{ 10, 170, 170,  10}};
inline const DogLegController::Pose DogLegController::kPoseBow     = {{140,  90, 140,  90}};
inline const DogLegController::Pose DogLegController::kPoseAlertL  = {{ 80,  80, 110, 110}};
inline const DogLegController::Pose DogLegController::kPoseAlertR  = {{100, 100,  70,  70}};

// C6 的 LEDC 有 4 个 timer / 至少 6 个 channel。
// 四路舵机同频（50Hz），用 **1 个 timer 共享时基 + 4 个 channel**，
// 比一个 timer 一个 channel 更省资源，也保证四路严格同步。
static constexpr ledc_timer_t kServoTimer = LEDC_TIMER_0;
static constexpr ledc_channel_t kServoChannels[DogLegController::kServoCount] = {
    LEDC_CHANNEL_0, LEDC_CHANNEL_1, LEDC_CHANNEL_2, LEDC_CHANNEL_3,
};

// 20 位定时器：50Hz 下分辨率 = 20位 / (APB 80MHz / 20000Hz)
// 实际约 0.5µs 一档，SG90 只需 5~10µs 精度，线性映射足够。
DogLegController::DogLegController(const std::array<gpio_num_t, kServoCount>& pins) {
    for (int i = 0; i < kServoCount; i++) {
        pins_[i]     = pins[i];
        channels_[i] = kServoChannels[i];
        current_[i]  = kMidAngle;
    }

    // 定时器配置
    ledc_timer_config_t timer_config = {};
    timer_config.speed_mode      = LEDC_LOW_SPEED_MODE;
    timer_config.duty_resolution = LEDC_TIMER_20_BIT;
    timer_config.timer_num       = kServoTimer;
    timer_config.freq_hz         = 50;
    timer_config.clk_cfg         = LEDC_AUTO_CLK;
    esp_err_t err = ledc_timer_config(&timer_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_DOG, "ledc_timer_config failed: %s", esp_err_to_name(err));
        return;
    }

    // 四路 channel
    for (int i = 0; i < kServoCount; i++) {
        ledc_channel_config_t ch_config = {};
        ch_config.gpio_num   = pins_[i];
        ch_config.speed_mode = LEDC_LOW_SPEED_MODE;
        ch_config.channel    = channels_[i];
        ch_config.timer_sel  = kServoTimer;
        ch_config.duty       = AngleToDuty((float)kMidAngle);
        ch_config.hpoint     = 0;
        err = ledc_channel_config(&ch_config);
        if (err != ESP_OK) {
            ESP_LOGE(TAG_DOG, "ledc_channel_config(%d, gpio=%d) failed: %s",
                     i, pins_[i], esp_err_to_name(err));
            return;
        }
    }

    // 关键安全步骤：先让四个舵机全部回 90° 中位，并等它们真的到位，
    // 再考虑别的。这样上电瞬间不会四个舵机同时猛冲到各自限位堵转发热。
    for (int i = 0; i < kServoCount; i++) {
        WriteOne(i, kMidAngle);
    }
    vTaskDelay(pdMS_TO_TICKS(kPowerOnSettleMs));

    ready_ = true;
    ESP_LOGI(TAG_DOG, "ready: 4x SG90 @50Hz 500-2500us, pins=%d,%d,%d,%d",
             pins_[0], pins_[1], pins_[2], pins_[3]);
    ESP_LOGI(TAG_DOG, "servo limits: %d..%d deg (mid=%d)", kMinAngle, kMaxAngle, kMidAngle);
    // 打印实际送进 LEDC 的 duty 计数，方便对着波形核对换算是否正确
    ESP_LOGI(TAG_DOG, "duty(2^%d scale): 0deg=%u 90deg=%u 170deg=%u",
             kDutyResBits,
             AngleToDuty(0.0f), AngleToDuty((float)kMidAngle), AngleToDuty((float)kMaxAngle));

    RegisterMcpTools();

#if DOG_AUTO_CYCLE
    StartAutoCycle();
#endif
}

std::string DogLegController::StateJson() const {
    char buf[192];
    snprintf(buf, sizeof(buf),
             "{\"ready\":%s,\"current_angles\":[%d,%d,%d,%d],"
             "\"min_limit\":%d,\"max_limit\":%d}",
             ready_ ? "true" : "false",
             current_[0], current_[1], current_[2], current_[3],
             kMinAngle, kMaxAngle);
    return std::string(buf);
}

std::vector<std::string> DogLegController::SupportedActions() {
    return {
        "stand",       // 站立 / 回到中立姿态
        "sit",         // 坐下然后回来
        "lie",         // 趴下
        "sleep",       // 睡觉（收成一团）
        "wake",        // 醒来（回站立）
        "bow",         // 鞠躬（打招呼/道谢）
        "alert",       // 警觉（竖起听指令）
        "wag",         // 摇身（开心/兴奋）
        "raise_left",  // 举左前腿
        "raise_right", // 举右前腿
        "front",       // 向前走一步
        "back",        // 向后退一步
        "turn_left",   // 左转
        "turn_right",  // 右转
    };
}

bool DogLegController::DoAction(const std::string& action, int repeat) {
    if (!ready_) {
        ESP_LOGW(TAG_DOG, "not ready, ignore action=%s", action.c_str());
        return false;
    }
    ESP_LOGI(TAG_DOG, "action=%s repeat=%d", action.c_str(), repeat);

    if      (action == "stand")       Stand();
    else if (action == "sit")         SitDown();
    else if (action == "lie")         Lie();
    else if (action == "sleep")       Sleep();
    else if (action == "wake")        Wake();
    else if (action == "bow")         Bow();
    else if (action == "alert")       Alert();
    else if (action == "wag")         Wag(repeat);
    else if (action == "raise_left")  RaiseLeft(repeat);
    else if (action == "raise_right") RaiseRight(repeat);
    else if (action == "front")       Front(repeat);
    else if (action == "back")        Back(repeat);
    else if (action == "turn_left")   TurnLeft(repeat);
    else if (action == "turn_right")  TurnRight(repeat);
    else {
        ESP_LOGW(TAG_DOG, "unknown action=%s", action.c_str());
        return false;
    }
    return true;
}

// ===== 安装标定：舵机臂装歪时在线对齐，不用重烧固件 =====

// 腿名映射。序号固定为 {RF, RB, LF, LB}，与 config.h 的 SERVO_GPIO_* 顺序一致。
static const char* kLegNames[DogLegController::kServoCount] = {
    "front_right", "back_right", "front_left", "back_left",
};

const char* DogLegController::LegName(int i) {
    if (i < 0 || i >= kServoCount) return "?";
    return kLegNames[i];
}

int DogLegController::LegIndexFromName(const std::string& raw) {
    std::string n;
    for (char c : raw) {
        n.push_back((char)std::tolower((unsigned char)c));
    }
    // 去掉分隔符，容忍 "front-left" / "Front Left" / "front_left"
    std::string k;
    for (char c : n) {
        if (c != '-' && c != '_' && c != ' ') k.push_back(c);
    }
    if (k == "fr" || k == "rf" || k == "rightfront" || k == "frontright") return 0;
    if (k == "br" || k == "rb" || k == "backright"  || k == "rearright") return 1;
    if (k == "fl" || k == "lf" || k == "leftfront"  || k == "frontleft")  return 2;
    if (k == "bl" || k == "lb" || k == "backleft"   || k == "rearleft")  return 3;
    // 中文直接映射：右前/右后/左前/左后
    if (raw.find("右前") != std::string::npos || raw.find("右前腿") != std::string::npos) return 0;
    if (raw.find("右后") != std::string::npos) return 1;
    if (raw.find("左前") != std::string::npos) return 2;
    if (raw.find("左后") != std::string::npos) return 3;
    if (k == "0") return 0;
    if (k == "1") return 1;
    if (k == "2") return 2;
    if (k == "3") return 3;
    return -1;
}

bool DogLegController::ProbeLeg(int i, int deg) {
    if (!ready_ || i < 0 || i >= kServoCount) return false;
    GlideOne(i, deg);
    ESP_LOGI(TAG_DOG, "probe %s -> logical %d deg (offset %+d => actual %d)",
             LegName(i), ClampAngle(deg), offset_[i], ApplyOffset(i, deg));
    return true;
}

int DogLegController::AdjustOffset(int i, int delta) {
    if (i < 0 || i >= kServoCount) return 0;
    // 偏置本身也夹到 ±40 度，防止误操作把某条腿彻底打歪
    offset_[i] = std::max(-40, std::min(40, offset_[i] + delta));
    // 偏置改了以后，用当前逻辑角度重新下发一次，立刻看到效果
    int logical = current_[i];
    current_[i] = ApplyOffset(i, logical);   // 临时把 current_ 推到位
    WriteOne(i, logical);
    ESP_LOGI(TAG_DOG, "offset %s = %+d deg (delta %+d, actual now %d)",
             LegName(i), offset_[i], delta, current_[i]);
    return offset_[i];
}

bool DogLegController::SetOffset(int i, int deg) {
    if (i < 0 || i >= kServoCount) return false;
    offset_[i] = std::max(-40, std::min(40, deg));
    int logical = current_[i];
    WriteOne(i, logical);
    ESP_LOGI(TAG_DOG, "offset %s set to %+d deg", LegName(i), offset_[i]);
    return true;
}

void DogLegController::RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();

    // 说明文字里把支持的 action 列全，AI 才不会瞎猜。
    std::string desc =
        "Control this four-legged robot dog's legs and body posture via a named action.\n"
        "Use this tool when the user says something like: 站起来 / 坐下 / 趴下 / 睡觉 / "
        "打个招呼 / 鞠个躬 / 摇摇身子 / 走走看 / 转个圈, or in English: stand / sit / lie down / "
        "bow / wag / walk / turn.\n"
        "IMPORTANT: only named actions are supported - you cannot pass raw servo angles.\n"
        "Available actions:\n";
    for (const auto& a : SupportedActions()) {
        desc += "  - " + a + "\n";
    }
    desc +=
        "\nUsage guidance:\n"
        "  * 'stand' is the safe neutral posture. Use it to reset between actions.\n"
        "  * Use 'wag' or 'bow' to express emotion (happy / grateful) in conversation.\n"
        "  * Use 'alert' when the user addresses you to show you are listening.\n"
        "  * 'front'/'back'/'turn_left'/'turn_right' make the dog move in place (it cannot "
        "translate across a desk), so pair them with a sentence like wagging while stepping.\n"
        "  * repeat controls how many cycles a repeating action (wag, walk, turn) performs.\n"
        "  * Do not call two actions back to back without a pause; each action already "
        "includes its own smooth interpolation.";

    mcp.AddTool("self.dog.do_action", desc,
                PropertyList({
                    Property("action", kPropertyTypeString),                      // 必填
                    Property("repeat", kPropertyTypeInteger, 1, 1, 5),           // 默认 1，范围 1..5
                }),
                [this](const PropertyList& properties) -> ReturnValue {
                    auto action = properties["action"].value<std::string>();
                    int repeat  = properties["repeat"].value<int>();
                    bool ok = DoAction(action, repeat);
                    if (!ok) {
                        std::string msg = "Unknown or unavailable action: " + action;
                        return msg;
                    }
                    return std::string("done: ") + action;
                });

    mcp.AddTool("self.dog.get_state",
                "Get the current servo angles and safe angle limits of the robot dog. "
                "Use this before acting if you are unsure whether the legs are powered on.",
                PropertyList(),
                [this](const PropertyList& properties) -> ReturnValue {
                    return StateJson();
                });

    // ===== 安装标定工具（舵机臂装歪 / 方向反了时用）=====
    mcp.AddTool("self.dog.calibrate",
                "Adjust the mechanical alignment of one leg without recompiling firmware.\n"
                "Use this ONLY when a servo arm is mounted at the wrong angle, so the legs do not "
                "look level or symmetric even in 'stand'.\n"
                "Legs are: front_right (0), back_right (1), front_left (2), back_left (3).\n"
                "Operations:\n"
                "  * op='probe'  -> move one leg to a raw angle (args: leg, angle) to find its zero.\n"
                "  * op='trim'   -> add degrees to that leg's mount offset (args: leg, delta).\n"
                "  * op='set'    -> set that leg's mount offset absolutely (args: leg, offset).\n"
                "  * op='read'   -> read all four offsets (args: none).\n"
                "  * op='reset'  -> clear all offsets back to 0 (args: none).\n"
                "Guidance: if the user says the front-left leg is 8 degrees too high, call "
                "op='trim', leg='front_left', delta=-8. If a leg moves the wrong way when the "
                "dog stands, use op='set' with offset=180 to flip it. Keep deltas small (<=10).",
                PropertyList({
                    Property("op",     kPropertyTypeString),                    // 必填
                    Property("leg",    kPropertyTypeString, ""),                // 默认空
                    Property("angle",  kPropertyTypeInteger, 90, kMinAngle, kMaxAngle),
                    Property("delta",  kPropertyTypeInteger, 0, -40, 40),
                    Property("offset", kPropertyTypeInteger, 0, -40, 40),
                }),
                [this](const PropertyList& properties) -> ReturnValue {
                    std::string op = properties["op"].value<std::string>();
                    for (auto& c : op) c = (char)std::tolower((unsigned char)c);

                    if (op == "read" || op == "reset") {
                        if (op == "reset") {
                            for (int i = 0; i < kServoCount; i++) {
                                offset_[i] = 0;
                                WriteOne(i, current_[i]);
                            }
                            ESP_LOGI(TAG_DOG, "all offsets reset to 0");
                        }
                        char buf[160];
                        snprintf(buf, sizeof(buf),
                                 "{\"offsets\":{\"%s\":%d,\"%s\":%d,\"%s\":%d,\"%s\":%d},"
                                 "\"current\":[%d,%d,%d,%d]}",
                                 kLegNames[0], offset_[0], kLegNames[1], offset_[1],
                                 kLegNames[2], offset_[2], kLegNames[3], offset_[3],
                                 current_[0], current_[1], current_[2], current_[3]);
                        return std::string(buf);
                    }

                    std::string leg = properties["leg"].value<std::string>();
                    int idx = LegIndexFromName(leg);
                    if (idx < 0) {
                        return std::string("Unknown leg: '") + leg +
                               "'. Valid: front_right / back_right / front_left / back_left "
                               "(or 右前/右后/左前/左后).";
                    }

                    if (op == "probe") {
                        int angle = properties["angle"].value<int>();
                        ProbeLeg(idx, angle);
                        return std::string("moved ") + LegName(idx) + " to " +
                               std::to_string(angle) + " deg";
                    }
                    if (op == "trim") {
                        int delta = properties["delta"].value<int>();
                        int now = AdjustOffset(idx, delta);
                        return std::string(LegName(idx)) + " offset is now " +
                               std::to_string(now) + " deg";
                    }
                    if (op == "set") {
                        int off = properties["offset"].value<int>();
                        SetOffset(idx, off);
                        return std::string(LegName(idx)) + " offset set to " +
                               std::to_string(off) + " deg";
                    }
                    return std::string("Unknown op: '") + op +
                           "'. Use probe / trim / set / read / reset.";
                });
}

// ===== 开机自动循环摆腿（调试用）=====
// 后台 FreeRTOS 任务：先等几秒让主固件稳定/联网，然后循环切换有明显角度
// 变化的动作，方便一眼确认舵机硬件是否通路。每轮之间回一次站立中位。
#if DOG_AUTO_CYCLE
void DogLegController::StartAutoCycle() {
    // 用动态分配的名字避免 xTaskCreate 对静态字符串的生命周期顾虑
    BaseType_t ok = xTaskCreate(AutoCycleTaskTrampoline,
                                "dog_auto_cycle",
                                4096,
                                this,
                                5,      // 优先级，低于音频/网络
                                nullptr);
    if (ok != pdPASS) {
        ESP_LOGE(TAG_DOG, "failed to create auto-cycle task");
    } else {
        ESP_LOGI(TAG_DOG, "auto-cycle task started (debug mode)");
    }
}

void DogLegController::AutoCycleTaskTrampoline(void* arg) {
    static_cast<DogLegController*>(arg)->AutoCycleTask();
    vTaskDelete(nullptr);
}

void DogLegController::AutoCycleTask() {
    // 启动延迟：等主固件稳定、网络连上，避免抢占启动日志
    vTaskDelay(pdMS_TO_TICKS(5000));

    // 一轮里依次展示这些动作，每个都是肉眼可见的大角度变化
    // 顺序特意让相邻动作差异大，方便观察舵机是否真在响应
    const std::vector<std::string> kCycle = {
        "sit",        // 坐下：右前 140 / 左前 40
        "stand",      // 回中位
        "lie",        // 趴下：170/10/10/170 全幅
        "stand",
        "raise_left", // 举左前腿
        "stand",
        "raise_right",// 举右前腿
        "stand",
        "bow",        // 鞠躬
        "stand",
        "wag",        // 摇身
        "stand",
        "turn_left",  // 左转
        "stand",
        "turn_right", // 右转
        "stand",
    };

    ESP_LOGI(TAG_DOG, "auto_cycle: entering loop with %u poses", (unsigned)kCycle.size());
    while (true) {
        for (const auto& a : kCycle) {
            if (!ready_) {
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            ESP_LOGI(TAG_DOG, "auto_cycle: -> %s", a.c_str());
            DoAction(a, 1);
            // 每个动作之后停顿，让人看清姿态
            vTaskDelay(pdMS_TO_TICKS(2500));
        }
    }
}
#endif  // DOG_AUTO_CYCLE

#endif  // __DOG_LEG_CONTROLLER_H__