import os

p = r'<repo>\.workbuddy\memory\2026-10-16.md'

add = """
## 新增 self.dog.calibrate 在线标定工具（22:45）

**动机**：舵机臂机械安装位置不对（用户反馈明天要调），但反复拆装机械很麻烦。
用软件偏置补偿对齐，比改机械效率高得多，且不用重烧固件。

**代码改动**（`main/boards/common/dog_leg_controller.h`）：
1. 新增成员 `int offset_[kServoCount] = {0,0,0,0};` —— 每条腿的安装偏置（度）
2. 新增 `int ApplyOffset(int i, int deg) const { return ClampAngle(deg + offset_[i]); }`
3. `WriteOne()` 改为下发 `AngleToDuty(ApplyOffset(i, deg))`；
   **`current_[]` 仍记录逻辑角度**（姿态库原值），插值计算不受偏置干扰
4. 新增公共方法：`ProbeLeg(i,deg)` / `AdjustOffset(i,delta)` / `SetOffset(i,deg)` /
   `GetOffset(i)` / `LegIndexFromName(name)` / `LegName(i)`
5. `LegIndexFromName` 支持 front_right/back_right/front_left/back_left、
   缩写 fr/br/fl/bl、以及中文 右前/右后/左前/左后
6. 新增 MCP 工具 `self.dog.calibrate`，5 个 op：
   - probe(leg, angle) 单腿试角度
   - trim(leg, delta) 增量调偏置
   - set(leg, offset) 绝对设偏置
   - read 读四条腿偏置
   - reset 全部归零
7. 补 `#include <cctype>`（tolower）和 `<cstdio>`（snprintf）
8. 偏置自身夹在 ±40°，防误操作把腿打歪

**编译**：`BUILD_EXIT=0`，bin = 2,325,904 字节（比无标定版 +4,560B）。
**未烧录** —— 等明天调好舵机臂后再一起烧，避免今晚再折腾硬件。

**明日收尾清单**（已写进 docs/2026-10-16-servo-dog-progress.md）：
1. 断电重装舵机臂，上电说"站起来"看是否水平
2. 判断嗡嗡声性质（摸温度：明显发烫=堵转=立刻断电；轻微均匀=正常闭环保持音）
3. 用 self.dog.calibrate 做软件偏置对齐
4. 把最终四个偏置值固化进 offset_[] 数组

**今日沉淀 skill**：用户级 `esp32-servo-pwm-troubleshooting`
（duty 量纲错误 / 外设占用假成功 / C6 引脚禁区 / 共地 / 单次连接烧录 / DTR-RTS 复位）
"""

with open(p, 'a', encoding='utf-8') as f:
    f.write(add)
print('appended. new size =', os.path.getsize(p))
