p = r'<repo>\.workbuddy\memory\2026-10-16.md'
s = open(p, encoding='utf-8').read()

old = '''- **左前 GPIO22 ✅动 / 左后 GPIO23 ✅ 动 / 右前 GPIO3 ✅ 动 / 右后 GPIO18 ❌ 不动**
- **根因 100% 确认是 duty 量纲 bug**，用户的接线、共地、DC模块供电**全部正确**。'''

new = '''- 左前 GPIO22 ✅ / 左后 GPIO23 ✅ / 右前 GPIO3 ✅ / 右后 GPIO18 起初 ❌
- **根因 100% 确认是 duty 量纲 bug**，用户的接线、共地、DC 模块供电**全部正确**。

### GPIO18 那一路：是面包板接触不良，不是外设占用（22:27 定论）

我一度判定是 **USB Serial/JTAG 外设占用**（ESP32-C6 该外设硬件挂 GPIO18/19，
sdkconfig 里 `CONFIG_USJ_ENABLE_USB_SERIAL_JTAG=y`），已把 config.h 改成 GPIO14 并编译了一次。
**用户反馈：他只是把面包板插线孔换到同一排的另一个孔，舵机就动了。**
→ 实为**面包板孔位接触不良**，GPIO18 保留不变，**GPIO14 的改动已撤回**。
→ 以后该路再不动：先换插孔 / 换杜邦线，最后才考虑迁GPIO14。

**方法论教训**：遇到"某一路不动、其他路正常"，**先用交换线缆法**（把该路信号线
临时插到一个已知正常的脚）区分「线/插孔问题」与「引脚被外设占用」——
插到好脚上能动就说明是线的问题。别急着改代码迁脚。
另外：LEDC `ledc_channel_config` 返回 `ESP_OK`、日志照常打印 ready，
**不代表引脚真的会输出脉冲**，这是最容易被"软件全绿"误导的地方。

### 自动循环已关闭（正式版）

`DOG_AUTO_CYCLE` 已从 1 改回 **0**，恢复"只听对话指令才动"，
否则固件会一直自己动，没法做语音联调。'''

s = s.replace(old, new, 1)
open(p, 'w', encoding='utf-8').write(s)
print('ok, size', len(s))