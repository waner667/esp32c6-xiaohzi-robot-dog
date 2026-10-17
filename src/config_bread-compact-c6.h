#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

// ESP32-C6 只有一个 I2S 控制器，因此必须使用 Duplex（共享时钟）接法：
// 麦克风 SCK 与功放 BCLK 共接 GPIO5；麦克风 WS 与功放 LRCK 共接 GPIO4。
// 采样率与 TF1990 的 C6 分支保持一致（输入输出同为 16000，避免输出侧重采样）：
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE AUDIO_INPUT_SAMPLE_RATE

#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_5
#define AUDIO_I2S_GPIO_WS   GPIO_NUM_4
#define AUDIO_I2S_GPIO_DIN  GPIO_NUM_6
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_7

// 引脚定义对齐 TF1990《小智面包板：ESP32C6硬件连线及固件分享》的接线方案：
//   按键 SPEAK=21 / VOL+=20 / VOL-=19；OLED SDA=11 / SCL=10（2026-10-14 按实测接线修正）
// 自己的板子丝印证实引出了 19/20/21/10/11，接线与本文件一致。
#define BUILTIN_LED_GPIO        GPIO_NUM_2
#define BOOT_BUTTON_GPIO        GPIO_NUM_9
#define TOUCH_BUTTON_GPIO       GPIO_NUM_21
#define VOLUME_UP_BUTTON_GPIO   GPIO_NUM_20
#define VOLUME_DOWN_BUTTON_GPIO GPIO_NUM_19

// 2026-10-14：实测接线为 SDA->GPIO11 / SCK(GPIO11 号)->GPIO10，与最初假设相反，
// 四脚 OLED 模块两个信号脚紧邻，接反概率很高。这里跟随实际接线对调。
#define DISPLAY_SDA_PIN GPIO_NUM_11
#define DISPLAY_SCL_PIN GPIO_NUM_10
#define DISPLAY_WIDTH   128

#if CONFIG_OLED_SSD1306_128X32
#define DISPLAY_HEIGHT  32
#elif CONFIG_OLED_SSD1306_128X64
#define DISPLAY_HEIGHT  64
#elif CONFIG_OLED_SH1106_128X64
#define DISPLAY_HEIGHT  64
#define SH1106
#else
#error "OLED display type is not selected"
#endif

#define DISPLAY_MIRROR_X true
#define DISPLAY_MIRROR_Y true


// A MCP Test: Control a lamp
#define LAMP_GPIO GPIO_NUM_1

// 四足桌宠舵机（SG90 × 4，2026-10-16）
//
// 引脚挑选依据（ESP32-C6，逐个避开禁区）：
//   已占用：1(LAMP) 2(LED) 4/5/6/7(I2S) 9(BOOT) 10/11(OLED) 19/20/21(按键)
//   不可用：24-30(内部 SPI flash) 16/17(UART0 串口日志) 8(strapping) 0(strapping)
//   → 剩下 3 / 14 / 15 / 18 / 22 / 23 可用，取其中分散的四个。
//
// ★ 排查留档（2026-10-16）：舵机2(GPIO18) 最初完全不动，一度怀疑是
//   ESP32-C6 的 USB Serial/JTAG 外设占用（该外设硬件挂GPIO18/19，
//   且 sdkconfig 里 CONFIG_USJ_ENABLE_USB_SERIAL_JTAG=y）。
//   但用户把面包板插孔换到同排另一个孔后舵机就动了 → **实为面包板孔接触不良**，
//   GPIO18 保留不变。若日后该路又不动，可先换插孔/换杜邦线，再考虑迁到 GPIO14。
//
// 左右镜像安装：左腿（舵机3/4）与右腿（舵机1/2）摆臂朝向身体中轴线对称，
// 代码里对镜像的一对腿用 右 = 180 - 左 处理。
#define SERVO_GPIO_RF GPIO_NUM_3   // 舵机1 右前腿
#define SERVO_GPIO_RB GPIO_NUM_18  // 舵机2 右后腿
#define SERVO_GPIO_LF GPIO_NUM_22  // 舵机3 左前腿
#define SERVO_GPIO_LB GPIO_NUM_23  // 舵机4 左后腿

// 按键 GPIO 原始电平探针（诊断用）。
//
// 2026-10-17：按键"没反应"的根因排查全靠它 —— 开机基线电平直接给出
// `boot(GPIO9)=1  touch(GPIO21)=0  vol+(GPIO20)=0  vol-(GPIO19)=0`，
// 说明三路从开机起就一直被接地（真根因：三脚微动开关 A|B|C 里外侧两脚
// 同属一端，按二脚思路接等于把同一端短接自己）。
//
// 正式固件默认关闭（避免每次按键都刷串口），需要排查时置 1 重编译即可。
#define BUTTON_GPIO_PROBE 0

#endif // _BOARD_CONFIG_H_
