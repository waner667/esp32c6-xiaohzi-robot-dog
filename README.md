# ESP32-C6 小智机器狗（四足舵机桌宠）

在[小智 AI 固件](https://github.com/waner667/esp32c6-xiaozhi-ai)基础上扩展的四足桌宠：
4 个 SG90 舵机作为四条腿，通过语音指令切换姿态（站立 / 坐下 / 趴下 / 睡觉 / 鞠躬 / 摇尾 / 前后左右移动）。

上层小智固件不在本仓库，见 `waner667/esp32c6-xiaozhi-ai`。本仓库只放**机器狗扩展部分**。

---

## 硬件

| 项 | 规格 |
|---|---|
| 主控 | ESP32-C6（TF1990C6 小智板，16MB Flash） |
| 舵机 | SG90 × 4（180° 版，4.8~6V，堵转约 0.65A/个） |
| 舵机供电 | **独立外部 5V**（≥3A），不可由ESP32 的 5V 引脚供电 |
| 电机驱动 | LEDC PWM，20-bit分辨率，50Hz |
| 音频 | 板载I2S 喇叭 + 麦克风 |

### 舵机引脚

| 舵机 | 位置 | GPIO | 备注 |
|---|---|---|---|
| 1 | 右前腿 | **3** | |
| 2 | 右后腿 | **18** | ESP32-C6 面包板孔位易接触不良，务必插紧 |
| 3 | 左前腿 | **22** | |
| 4 | 左后腿 | **23** | |

### ESP32-C6 引脚禁区（选脚必看）

| 脚 | 原因 |
|---|---|
| 0 / 8 | strapping（上下电电平决定启动行为） |
| 16 / 17 | UART0 串口日志 |
| **18 / 19** | **USB Serial/JTAG 硬件占用**（`CONFIG_USJ_ENABLE_USB_SERIAL_JTAG=y`） |
| 24~ 30 | 芯片内部 SPI flash |
| 1 / 2 / 4~ 7 / 9~ 11 / 19~ 21 | 本板已被灯 / I2S 音频 / OLED / 按键占用 |

> ⚠️ **「ledc/gpio 配置返回 ESP_OK」不代表引脚真的有输出。**
> 某脚"初始化成功但无输出"时，第一反应应该是查它有没有被芯片内部外设占用。

---

## 接线

```
外部 5V 电源 OUT+  ──┬── 舵机1 红线
                     ├── 舵机2 红线
                     ├── 舵机3 红线
                     └── 舵机4 红线

外部 5V 电源 OUT−  ──┬── 舵机1~4 棕线
                     └── ESP32 任意 GND   ← ★ 共地线，不能少

ESP32 GPIO3/18/22/23 ── 舵机1~4 橙/白色信号线
```

**共地为什么必需**：PWM 脉冲的实际高度 = `V(信号) − V(GND)`。
两边的 GND 不互连时，信号参考电平会漂移，舵机收不到有效脉冲 → 不动或乱抖。

**供电注意**
- 4 个 SG90 最坏总电流约 2.6A，电源必须留余量
- **不要走面包板电源轨**（每段通常只能安全过 1A，接触电阻大）
- 电源输出调 **5.0V**，超过 6V 会烧舵机内部驱动芯片
- DC 模块输出低电压时会触发电流限制，电机反复挣扎发出"滋滋"声 —— 调高电压即消失

---

## 动作清单

MCP 只暴露语义动作，不暴露裸角度（避免 AI 传出越界值）。

| action | 姿态 | 说明 |
|---|---|---|
| `stand` | 90/90/90/90 | 站立基准，也是上电回中位 |
| `sit` | 前腿收后腿撑 | 坐下 |
| `lie` | 170/10/10/170 | 趴下，幅度最大 |
| `sleep` | 缩成一团 | |
| `bow` | 前后摆 | 鞠躬 |
| `wag` | 左右摆 3 次 | 摇尾巴 |
| `raise_left` / `raise_right` | 单腿抬起 | |
| `forward` / `backward` | 对角步态原地踏步 | |
| `turn_left` / `turn_right` | 对角步态原地踏步 | |

### MCP 工具

| 工具 | 参数 | 说明 |
|---|---|---|
| `self.dog.do_action` | `action`、`repeat`(1~5) | 执行语义动作 |
| `self.dog.get_state` | — | 读当前4 路角度与安全限位 |
| `self.dog.calibrate` | `op` = `probe`/`trim`/`set`/`read`/`reset` | 安装偏置标定（见下） |

---

## 安全设计

- **硬限位 10°~ 170°**：任何角度写入前夹紧，防堵转发热
- **上电自动回90° + 300ms 稳定延时**：避免上电瞬间猛冲
- **5° / 20ms 平滑插值**：所有姿态切换都是渐变，不会瞬间甩腿
- **MCP 只暴露语义动作**：AI 无法传出裸角度

### 装舵机臂的顺序（重要）

1. **先断电**
2. 断电状态下装臂 —— 通电时强行扳舵机臂 = 命令它转它不转 → 电机堵转 → 几分钟烧掉减速齿轮
3. 装完再上电，上电瞬间舵机自动回 90°
4. 若臂不在 90° 对应位置，说明装歪了 → 断电重装

装机械臂的基准就是 `stand` = 90°，上下各有 80° 余量，任何姿态都不会撞机械限位。

### 安装偏置标定

若某条腿装得偏高/偏低，用软件偏置补偿，不用反复拆机械：

- 对小智说「把左前腿调高 8 度」→ 加 +8° 偏置
- 「读一下四条腿的偏置」→ 查看当前值
- 「重置所有偏置」→ 归零

偏置自身夹紧 ±40°。调好后把 `dog_leg_controller.h` 里 `offset_[]` 四个值固化进代码即可。

---

## PWM duty换算（重点，踩过大坑）

ESP-IDF 的 `ledc_set_duty()` 接收的是**计数值**，范围 `[0, 2^duty_resolution]`，
**不是微秒数**。20 位分辨率下计数范围 `0 ~ 1,048,575`。

正确换算：

```
duty = pulse_width_us × 2^20 / period_us = pulse_width_us × 1048576 / 20000
     = pulse_width_us × 52.43
```

| 角度 | 脉宽 | duty |
|---|---|---|
| 0° | 500µs | 26214 |
| 90° | 1500µs | 78643 |
| 170° | 2283µs | 125199 |

把微秒数直接当duty 传（传 1500）得到的实际脉冲只有 **28.6µs**，
远低于 SG90 的 500µs 识别阈值 → **4 个舵机全部"有电但不位移 + 滋滋响"**。

固件启动时会打印实际 duty 值，任何时候都能核对：

```
DogLegCtrl: duty(2^20 scale): 0deg=26214 90deg=78643 170deg=125199
```

---

## 烧录

### Flash 偏移表

| 偏移 | 文件 | 大小 |
|---|---|---|
| `0x0` | `bootloader.bin` | 16,784 |
| `0x8000` | `partition-table.bin` | 3,072 |
| `0xd000` | `ota_data_initial.bin` | 8,192 |
| `0x20000` | `xiaozhi.bin` | 2,325,904 |
| `0x800000` | `generated_assets.bin` | 395,920 |

flash mode = `dio`，size = `16MB`，freq = `80m`

### 命令

```powershell
# 编译
.\scripts\build_autocycle.ps1

# 烧录（单次连接，--after no-reset）
python .\scripts\_flash.py

# 触发复位启动
python .\scripts\_run.py

# 监听串口日志
python .\scripts\_watch_final.py
```

---

## 本机踩过的6 个坑

1. **串口芯片是 CH343（VID:PID `1A86:55D3`）**，COM 号会变。
   必须按 VID:PID 识别，别信端口号 —— COM3/4/5/6/8/9 全是蓝牙虚拟口，
   在错误端口上 esptool 会一直空等。

2. **C6 自动复位不工作**。`--before default_reset` 无效，必须手动进下载模式：
   按住 BOOT → 点一下 RST → 松 RST → **保持按住 BOOT 约 2 秒** → 松 BOOT。

3. **烧录必须单次连接**。先跑 `chip_id` 探测再跑 `write_flash` 会失败 ——
   探测成功后的硬复位把板子踢出了下载模式，第二次连不上。
   正确做法：`--before no_reset` + `--after no-reset` 一口气写完，再单独触发复位。

4. **读串口前必须先 `setDTR(False)` / `setRTS(False)`**。
   否则开串口的瞬间板子就被复位，开机日志永久丢失。

5. **`read_flash` 全片读不稳定**（串口丢字节），分块最多稳定读到约 2MB。
   **固件备份/回退一律走 git，不要折腾 read_flash。**

6. **本机 bash shim 缺命令**：`ls` / `cp` / `mkdir` / `head` / `tail` / `dirname` / `rm`
   都不可用，且 `rm` 会静默失败（报错被 `dirname` 缺失掩盖，exit code 仍为 0）。
   一切文件操作用 Python 脚本或 PowerShell 工具完成。

---

## 目录结构

```
phase2-servo-dog/
├── src/                     舵机扩展源码（从 SDK 抽出，可直接覆盖回工程）
│   ├── dog_leg_controller.h     ★ 核心：姿态库 + LEDC 初始化 + MCP 工具
│   ├── config_bread-compact-c6.h    舵机引脚宏定义
│   └── compact_c6_board.cc         板级文件（含 DogLegController 实例化）
├── firmware/                可直接烧录的 5 个分区产物
├── scripts/                 编译 / 烧录 / 串口抓取 / duty 校验脚本
├── docs/                    方案文档 + 全流程复盘
├── notes/                   排查过程记录脚本
├── backup/                  固件备份
└── logs/                    编译日志
```

### 源码如何放回 SDK 工程

`src/` 三个文件对应SDK 工程里的路径：

```
dog_leg_controller.h     →  main/boards/common/dog_leg_controller.h
config_bread-compact-c6.h →  main/boards/bread-compact-c6/config.h
compact_c6_board.cc         →  main/boards/bread-compact-c6/compact_c6_board.cc
```

`dog_leg_controller.h` 是 header-only，`boards/common/*.cc` 需要在 CMakeLists 里显式列出，
所以做成 header-only 直接 include，**不用改 CMakeLists**。

---

## 版本回退

SDK 工程已 git 化（分支 `servo-work`）：

```
8084d5a  修正 duty 量纲 + 增加安装标定工具
47695ee  四舵机 MCP 工具
97d0868  基线（tag: oled-working）← 一键回退到无舵机的 OLED 版
```

回退：`git checkout oled-working` 后重编译烧录。

---

## 调试开关

`dog_leg_controller.h` 顶部：

```c
#ifndef DOG_AUTO_CYCLE
#define DOG_AUTO_CYCLE 0
#endif
```

设为 `1` 时固件启动 5 秒后自动循环切换 16 个大角度姿态，**不依赖语音/MCP**，
用于现场确认舵机硬件（供电 / 共地 / 信号线）是否通路。调试完改回 `0`。
