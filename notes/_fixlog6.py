import os, datetime

p = r'<repo>\.workbuddy\memory\2026-10-16.md'

add = """
## 舵机全链路打通 + 烧录正式版（22:35收尾）

**硬件验证完成**：用户换 breadboard 插孔后 GPIO18 那一路也通了，4 个 SG90 全部
跟随自动循环动作正常运动（"4个舵机时不时的都会在动" = 跟着动作节奏，非堵转抖动）。

**最终版固件已烧录并运行**：
- `DOG_AUTO_CYCLE` = 0（自动循环关闭，恢复"只听指令"）
- SERVO_GPIO_RB 保持 GPIO_NUM_18（用户判断是 breadboard 孔位接触不良，非 USB Serial/JTAG 占用；
  config.h 里保留了这段排查留档注释，勿再改脚）
- bin = 2,321,344 字节（比调试版小 704B，正好是去掉 xTaskCreate 的自动循环任务）
- FLASH rc=0，5 分区 Hash verified；运行期心跳正常（free sram 162504 稳定，无 panic）

**串口监听要点**（_watch_final.py）：
开串口前必须 `ser.dtr=False; ser.rts=False`，否则开串口瞬间会复位板子，开机日志抓不到。
板子已启动后再监听只能抓到运行期日志（如 SystemInfo 心跳），开机行会丢失。

**下一步：语音联调**。先用大角度动作测（stand=90度与上电中位相同，肉眼看不出）：
sit / lie / sleep / wag / bow / raise_left / raise_right / turn_left / turn_right。
姿态库数值在 `main/boards/common/dog_leg_controller.h` 的 kPose* 常量，不对就直接改那里重编。
"""

with open(p, 'a', encoding='utf-8') as f:
    f.write(add)
print('appended. new size =', os.path.getsize(p))
