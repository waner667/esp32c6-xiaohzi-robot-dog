# 小智狗 · 四舵机扩展 —— 2026-10-16 全流程复盘

> 一天时间从"方案设计"走到"4 个舵机全部随语音指令正常运动"。
> 本文记录方案、代码、**踩过的 3 个坑及其根因**、接线规范，以及明天的收尾事项。

---

## 一、最终成果

| 项 | 状态 |
|---|---|
| 4 × SG90 舵机 | ✅ 全部随语音指令运动 |
| 语音链路（唤醒 → 识别 → AI 决策 → MCP 调用 → 舵机动作） | ✅ 全通 |
| 固件版本 | `xiaozhi-esp32-2.5.2`，bin = 2,325,904 字节 |
| git 分支 | `servo-work`（基线 tag `oled-working` 保留可回退） |
| 代码位置 | `main/boards/common/dog_leg_controller.h`（header-only） |

**遗留问题（明天处理）**：舵机臂机械安装位置需要调整；4 个舵机持续"嗡嗡"声，待判断是否堵转。

---

## 二、硬件与接线规范（务必照做）

### 供电 —— 舵机红线禁止接 ESP32 的 5V

```
外部 5V 电源正 (≥2A，建议 3A)
    └──→ 4 个舵机红线（并联）

外部 5V 电源负
    ├──→ 4 个舵机棕线（GND）
    └──→ ESP32 任意 GND 引脚   ★ 共地线，不能少

ESP32 GPIO3 / 18 / 22 / 23
    └──→ 4 个舵机的橙/白色信号线
```

**★ 共地是必须的。** PWM 脉冲的有效电平 = `V(信号) − V(GND)`。两边的 GND 不互连时，
各自浮动的参考电平会让舵机完全收不到有效脉冲（有电、有锁死阻力、但纹丝不动）。

### 引脚分配（ESP32-C6，避开所有禁区）

| 舵机 | 部位 | GPIO | 备注 |
|---|---|---|---|
| 舵机1 | 右前腿 RF | **GPIO3** | |
| 舵机2 | 右后腿 RB | **GPIO18** | |
| 舵机3 | 左前腿 LF | **GPIO22** | |
| 舵机4 | 左后腿 LB | **GPIO23** | |

**C6 禁区**（选脚时必须避开）：
- `0` / `8` —— strapping 引脚，上下电电平决定启动模式
- `16` / `17` —— UART0 串口日志
- `18` / `19` —— USB Serial/JTAG 硬件占用（`CONFIG_USJ_ENABLE_USB_SERIAL_JTAG=y`）
- `24`~`30` —— 芯片内部 SPI flash
- `1`(LAMP) / `2`(LED) / `4-7`(I2S 喇叭) / `9`(BOOT) / `10-11`(OLED) / `19-21`(按键) —— 已被本板占用

### 电压与电流

- SG90 额定 **4.8~6V，推荐 5.0V**，超 6V 会烧舵机内部芯片
- 静止约 20~30mA/个；堵转 300~600mA/个；4 个同时动作最坏 ~2.6A
- 电压调高时电流需求反而下降（低压下模块会触发电流限制，导致电机断续挣扎而"滋滋"响）

---

## 三、⚠️ 踩过的三个坑（都是真实代价换来的）

### 坑 1（最严重）：duty 量纲错误 —— 导致 4 个舵机全部纹丝不动

**现象**：固件正常启动、日志照常打印 `DogLegCtrl: ready`、舵机通电有锁死阻力、
上电时还有"滋滋"声 —— 但**一个角度指令都不动**。

**根因**：`AngleToDuty()` 把**微秒数直接当 duty 计数值**传给了 `ledc_set_duty()`。

ESP-IDF 头文件明确写：

> the range of duty setting is `[0, (2**duty_resolution)]`

20 位分辨率 → 计数范围 `0 ~ 1,048,575`。而我们传进去的是 `500 ~ 2500`，
于是真实脉冲宽度 = `1500 / 1048576 × 20000µs ≈ 28.6µs`，
**远低于 SG90 的 500µs 识别阈值** → 舵机收到一堆它看不懂的窄脉冲，
电机空转挣扎发出"滋滋"声，但驱动芯片锁死了（有阻力）。

**正确换算**：

```
duty = pulse_width_us × 2^resolution / period_us
     = pulse_width_us × 1048576 / 20000
     = pulse_width_us × 52.43
```

| 角度 | 脉冲 | duty |
|---|---|---|
| 0° | 500µs | 26214 |
| 90°（中位） | 1500µs | 78643 |
| 180° | 2500µs | 131072 |

**教训**：调用"设占空比"类 API 前，先确认它的**单位是时间还是计数值**。
现在构造函数会打印 `duty(2^20 scale): 0deg=26214 90deg=78643 170deg=125199`，随时可核对。

---

### 坑 2：舵机臂机械安装位置不对 —— 导致持续嗡嗡声（**明天处理**）

**现象**：4 个舵机能动，但安装后"不水平/不对称"，且**持续嗡嗡响**。

**两种嗡嗡声要分清**：

| 类型 | 判据 | 处置 |
|---|---|---|
| **正常保持音** ✅ | 轻微、均匀、低音量；外壳仅微温 | 无需处理 |
| **堵转报警** ❌ | 音量大/高频尖叫；外壳明显发烫；掰不动 | **立刻断电**，否则烧齿轮 |

堵转的根因是舵机臂撞到机械限位，或安装角度导致某个姿态下物理转不过去。

---

### 坑 3：舵机2（GPIO18）完全不动 —— 是面包板孔位接触不良

**现象**：左前/左后/右前都正常，只有接 GPIO18 的右后腿不动。

**排查过程**：曾怀疑是 `GPIO18` 被 USB Serial/JTAG 外设占用（`CONFIG_USJ_ENABLE_USB_SERIAL_JTAG=y`，
该外设硬件固定挂 GPIO18/GPIO19）→ 一度准备改到 GPIO14。
但**用户把信号线换到同一排的第 2 个插孔后就正常了** → 结论是 **breadboard 孔位夹不紧（接触不良）**，
GPIO18 保持不变。

**教训**：
1. 「外设配置返回 ESP_OK」**不代表引脚真的有输出**。若某脚"配置成功但无输出"，第一反应查外设占用表。
2. **硬件问题优先用物理判据验证**（换插孔 / 万用表量波形），不要急着改代码/改引脚。

---

## 四、代码改动清单

### 新增文件

**`main/boards/common/dog_leg_controller.h`**（header-only，约 600 行）

做成 header-only 的原因：`boards/common/*.cc` 需要在 CMakeLists 里显式列出，
新增 .cc 文件容易踩漏；header-only 则只要被 include 就会编译。

**核心设计**：

```cpp
// 引脚顺序：{RF, RB, LF, LB}
static constexpr uint32_t kServoPeriodUs = 20000;  // 50Hz
static constexpr uint32_t kPulseMinUs    = 500;    // 0°
static constexpr uint32_t kPulseMaxUs    = 2500;   // 180°
static constexpr int      kMidAngle      = 90;     // 站立基准
static constexpr int      kMinAngle      = 10;     // 硬限位下界（留余量防堵转）
static constexpr int      kMaxAngle      = 170;    // 硬限位上界
static constexpr int      kPowerOnSettleMs = 300;  // 上电回中位后等待
static constexpr int      kStepDegrees   = 5;      // 插值步长
static constexpr int      kStepDelayMs   = 20;     // 插值间隔
```

**LEDC 配置**：C6 有 4 个 timer / 至少 6 个 channel。用 **1 个 timer 共享时基 + 4 个 channel**，
比一 timer 一 channel 更省资源，且保证四路严格同步。

**安全设计（这几条都是烧硬件换来的，别删）**：
1. 硬夹限位 10°~170° —— SG90 顶到 0/180 会持续堵转发热
2. 上电不直接给目标角度，先回 90° 中位并等 300ms，让四腿同时到位
3. 分步插值 5°/20ms，避免瞬间大角度冲击
4. MCP **只暴露语义动作名**，不暴露裸角度（防止 AI 给出危险角度）

**安装偏置补偿（新增，为明天调舵机臂准备）**：

```cpp
int offset_[kServoCount] = {0, 0, 0, 0};   // 每条腿的机械安装偏差（度）

int ApplyOffset(int i, int deg) const {
    return ClampAngle(deg + offset_[i]);    // 先夹紧，再叠偏置，再夹紧
}
```

`current_[]` 记录**逻辑角度**（姿态库原值），只有 `WriteOne()` 下发给硬件时才叠偏置，
这样插值计算不会被偏置干扰。

**姿态库**（左右镜像：右腿角度 = 180 − 左腿角度）：

```cpp
kPoseStand   = {{ 90,  90,  90,  90}};   // 站立 = 上电中位
kPoseSitdown = {{140,  90,  40,  90}};   // 坐下
kPoseLie     = {{170,  10,  10, 170}};   // 趴下
kPoseSleep   = {{ 10, 170, 170,  10}};   // 睡觉
kPoseBow     = {{140,  90, 140,  90}};   // 鞠躬
kPoseAlertL  = {{ 80,  80, 110, 110}};   // 警觉（左耳竖起）
kPoseAlertR  = {{100, 100,  70,  70}};   // 警觉（右耳竖起）
```

**动作清单（14 个）**：
`stand / sit / lie / sleep / wake / bow / alert / wag / raise_left / raise_right /
front / back / turn_left / turn_right`

### MCP 工具

| 工具 | 用途 |
|---|---|
| `self.dog.do_action` | 执行语义动作（`action` + `repeat` 1~5） |
| `self.dog.get_state` | 读当前四路角度与限位 |
| `self.dog.calibrate` | **在线标定舵机臂**（新增，明天用） |

### 其他改动

- `main/boards/bread-compact-c6/config.h`：加 4 个 `SERVO_GPIO_*` 宏 + 引脚禁区排查注释留档
- `main/boards/bread-compact-c6/compact_c6_board.cc`：`InitializeTools()` 内
  `static DogLegController dog({SERVO_GPIO_RF, SERVO_GPIO_RB, SERVO_GPIO_LF, SERVO_GPIO_LB});`
- `build_servo2.ps1` / `build_autocycle.ps1`：编译脚本

### `self.dog.calibrate` 用法（明天调舵机臂）

**目的：舵机臂装的时候不可能四腿绝对水平，用软件偏置对齐，比反复拆装机械方便得多。**

| op | 参数 | 作用 |
|---|---|---|
| `probe` | `leg`, `angle` | 把某条腿送到指定逻辑角度，找零点 |
| `trim` | `leg`, `delta` | 给某条腿的安装偏置 ±delta 度 |
| `set` | `leg`, `offset` | 绝对设置该腿偏置 |
| `read` | — | 读四条腿当前偏置 |
| `reset` | — | 全部归零 |

腿名接受 `front_right` / `back_right` / `front_left` / `back_left`，也接受 `右前`/`右后`/`左前`/`左后`。

**语音示例**：
- "把左前腿调高 8 度" → `trim, leg=front_left, delta=-8`
- "右后腿的角度高了 5 度" → `trim, leg=back_right, delta=-5`
- "读一下四条腿的偏置" → `read`
- "重置所有偏置" → `reset`

⚠️ 偏置上限 ±40°（防误操作）。**调好后把四个值固化进 `offset_[]` 数组**，以后上电自动生效。

---

## 五、构建与烧录工作流

### 环境

```
IDF_PATH        = <IDF_PATH>
IDF_TOOLS_PATH  = <app>\ESP\Espressif
esptool         = <app>\ESP\Espressif\python_env\idf6.1_py3.13_env\Scripts\esptool.exe
python          = <home>\.workbuddy\binaries\python\versions\3.13.12\python.exe
```

### 编译

```powershell
.\build_autocycle.ps1        # 两阶段：先单文件验证，再全量
```

⚠️ ninja 单文件目标路径是 `esp-idf/main/CMakeFiles/__idf_main.dir/...`
（**不是 `main.dir`**，写错会报 unknown target）。

### 烧录（关键坑）

**① 串口芯片是 CH343，不是 CH340**

`COM10` = `USB-Enhanced-SERIAL CH343`，VID:PID = `1A86:55D3`。
COM3/4/5/6/8/9 全是蓝牙虚拟口，选错会让 esptool 在错误端口空等。

**② C6 自动复位不工作，必须手动进下载模式**

```
按住 BOOT → 点一下 RST → 松开 RST → 保持按住 BOOT 约 2 秒 → 松开 BOOT
```

**③ 烧录必须是"单次连接"**（`_flash.py`）

```
--before no-reset   （已手动进模式，别让它再复位）
--after  no-reset    （烧完不复位，保持 stub 在线）
```

❌ 不能拆成"先 `chip_id` 探测 + 再 `write_flash`"两次调用 ——
第一次探测的 `hard_reset` 会把板子踢出下载模式，第二次必然连不上。

✅ 正确流程：**单次 `write_flash`** → 再单独发一次 `run` 触发启动。

**④ 固件备份/回退走 git，不要折腾 `read_flash`**

`read_flash` 全片极不稳定（串口丢字节），分块最多稳定读到约 2MB。
git 基线：`97d0868`（tag `oled-working`），回退即 `git checkout oled-working` 后重编译烧录。

### 读串口日志

⚠️ **开串口前必须 `ser.dtr = False; ser.rts = False`**，否则开串口的瞬间会复位板子，
开机日志全部丢失。板子已启动后再监听只能抓到运行期日志。

---

## 六、明天要做的事

### ① 调整舵机臂安装位置

1. **先断电**（别让舵机闷着发热）
2. 重新安装 4 个舵机臂，让站立时四腿水平对称
3. 上电，说"站起来"看是否水平
4. 若仍有偏差，**直接用 `self.dog.calibrate` 做软件偏置补偿**（不用拆机械）

### ② 判断嗡嗡声性质

| 测试 | 判读 |
|---|---|
| 摸舵机外壳温度 | 明显发烫 → 堵转，检查机械限位 |
| 上电时掰舵机臂 | 纹丝不动 → 那个角度撞限位 |
| 断开红线只留棕线 | 不烫 → 纯供电/信号问题；还烫 → DC 模块问题 |

### ③ 固化偏置值

标定完成后，把 `offset_[]` 的四个值写死进代码，然后重新编译烧录正式版。

---

## 七、快速命令速查

```powershell
# 编译
.\build_autocycle.ps1

# 烧录（需先手动进下载模式）
python .\_flash.py

# 触发启动
python .\_run.py

# 监听串口日志
python .\_watch_final.py

# git 回退到可用基线
cd me\xiaozhi-esp32-2.5.2
git checkout oled-working
```

---

## 八、核心经验（可复用到其他 ESP32 项目）

1. **「外设配置返回 ESP_OK」≠「引脚真的有输出」** —— 若某脚无输出，第一反应查外设占用表。
2. **调用设占空比 API 前先确认单位** —— 是时间还是计数值？（今天最大的坑）
3. **硬件问题优先用物理判据验证**（换插孔、万用表量波形、摸温度），不要急着改代码。
4. **"通电但不动"的快速分诊**：
   - 完全无阻力 → 没通电（红线/棕线）
   - 有阻力但不位移 → PWM 无效（duty 量纲 / 引脚 / 共地）
   - 有位移但方向角度怪 → 逻辑错（极性 / 镜像关系）
5. **C6 引脚禁区**：0/8（strapping）、16/17（UART0）、**18/19（USB Serial/JTAG）**、24-30（flash）。
6. **语音联调时避免用"无变化"的动作做首次验证** —— `stand` = 90° = 上电中位，肉眼看不出，会误判成"没反应"。
