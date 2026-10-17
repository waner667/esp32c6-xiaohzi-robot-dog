p = r'<repo>\.workbuddy\memory\2026-10-16.md'
s = open(p, encoding='utf-8').read()

marker = '## 舵机不动的排查过程 + 自动摆腿调试固件（22:00）'
idx = s.find(marker)
head = s[:idx].rstrip() if idx != -1 else s.rstrip()

body = '''## ★ 舵机完全不动的真正根因：LEDC duty 量纲错误（22:15，已修复）

**这是本项目最贵的一个 bug** —— 害用户反复插拔硬件排查半天，根因是我写错了。

**Bug**：`AngleToDuty()` 把微秒数直接当 duty 传给 `ledc_set_duty()`。
```cpp
// 错：return (uint32_t)(kPulseMinUs + span * deg / 180.0f);   // 500~2500
```

**为什么错**（ESP-IDF `driver/ledc.h` 原文）：
> `@param duty Set the LEDC duty, the range of duty setting is [0, (2**duty_resolution)]`

duty 是**定时器计数值**，不是时间。2^20 = 1048576。
所以传 1500 得到的实际脉冲 = 1500/1048576*20000 = **28.6µs**，
而 SG90 的识别阈值是 **≥500µs** → 舵机收到一堆看不懂的窄脉冲，**完全不动**。
实测换算：0deg→9.54µs / 90deg→28.61µs / 180deg→47.68µs，全部远低于阈值。

**这完美解释了现场现象**：舵机**有阻力（红棕线有电，驱动芯片上电）但零位移** ——
所以我前面把所有注意力都误导到"共地缺失/信号线接错/供电不足"上，用户为此把模块全拆了两次。

**正确换算**：
```cpp
duty = pulse_width_us * (1UL << 20) / 20000 = pulse_width_us * 52.4288
```
| 角度 | 脉冲 | duty |
|---|---|---|
| 0deg | 500us | 26214 |
| 90deg | 1500us | 78643 |
| 180deg | 2500us | 131072 |
| 170deg | 2388us | 125199 |

已用 Python 验算回算误差 < 0.02µs。验证脚本 `_verify_duty.py`。

**修复内容**：
1. 新增常量 `kDutyResBits = 20`，`AngleToDuty()` 改为
   `pulse_us * (1UL << kDutyResBits) / kServoPeriodUs`
2. 注释里完整记录这个坑 + 三档对照表（防止以后再改错）
3. 构造函数加日志 `duty(2^20 scale): 0deg=%u 90deg=%u 170deg=%u`，上电可直接核对

**教训（重要，同类项目通用）**：
- ESP32 LEDC 的 duty **永远是 2^resolution 量纲的计数值**，
  微秒→duty 要除以周期再乘满量程。写 PWM 时**必须做反向验算**（duty→µs 还原对比）。
- 纯软件 bug 会伪装成硬件故障。现象是"设备通电但不动"时，
  **先用最简单的物理判据区分**（有没有阻力/有没有电流），再怀疑代码，
  不要一上来就引导用户改接线。
- 用户说"之前动过"要重视：我一开始轻信了这句话，没意识到可能是记忆混淆，
  导致在硬件方向上绕了远路。

**重新编译**：STAGE1_EXIT=0 / BUILD_EXIT=0，bin = 2,322,048 字节。

---

## 排查过程存档（22:00-22:15，供参考）

**现象**：烧 servo-work 固件后说"站起来"，小智回复成功，但舵机完全不动。

**已排除（软件侧全绿）**：固件崩溃（DogLegCtrl: ready 有打印）、网络问题（连上了，
只是慢 5s，RSSI -73）、MCP 未调用（小智确实回了成功）、引脚冲突（config.h 逐个核对，
3/18/22/23 全是空闲脚，24-30是flash/16-17是UART0/8和0是strapping，都避开了）。

**误导性线索**：用户说"上电后舵机有阻力"→ 被我解读成"舵机已通电"，从而排除供电问题、
把矛头指向共地/信号线。实际上"有阻力"只说明机械齿轮有摩擦阻力，**不能证明 PWM 被识别**。
这个误判让排查方向偏了两小时。

**加的调试固件**（`dog_leg_controller.h`，仍然保留，有用）：
- `#ifndef DOG_AUTO_CYCLE / #define DOG_AUTO_CYCLE 1` 开关
- `StartAutoCycle()` / `AutoCycleTaskTrampoline()` / `AutoCycleTask()`
- 构造函数末尾 `#if DOG_AUTO_CYCLE StartAutoCycle(); #endif`
- `xTaskCreate` 起独立任务（栈4096、prio 5、名字 dog_auto_cycle），延迟 5s后循环 16 步
  sit→stand→lie→stand→raise_left→stand→raise_right→stand→bow→stand→wag→stand
  →turn_left→stand→turn_right→stand，每步后停 2500ms
- 该固件烧进去后自动循环确实跑起来了（日志确认 16 poses 走完前10 步），
  **但舵机纹丝不动** —— 这才最终指向代码而非硬件

**硬件问题（次要，但确实存在）**：
- 面包板电源轨载流有限，4个 SG90 最坏 2.6A，建议红线不经过面包板直接并到DC模块 OUT+。
- 喇叭功放和舵机共用 DC 模块时，音量冲击可能造成压降，排查时可临时拔掉喇叭。
'''

open(p, 'w', encoding='utf-8').write(head + '\n\n' + body)
print('rewritten, size', len(open(p, encoding="utf-8").read()))