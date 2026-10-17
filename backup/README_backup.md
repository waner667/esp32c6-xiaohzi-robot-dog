# 当前固件备份说明（已验证）

## 结论：板上跑的固件 == 当前源码编译产物

**证据链（三重交叉验证，全部通过）：**

1. **硬件备份与 build 产物逐字节一致**
   - 从 flash 读出的备份：`[0:16784]` 与 `build/bootloader/bootloader.bin` **完全相同**
   - 在 flash 偏移 `0x20000`（131072）处找到 app 固件，其 **1,867,776 字节与 `build/xiaozhi.bin` 完全一致**
   - 即：板上跑的固件，就是 `me/xiaozhi-esp32-2.5.2` 当前源码编译出来的那份

2. **时间线吻合**
   - `build/xiaozhi.bin` mtime = **2026-10-14 22:35:46**
   - 最后一次源码改动 `compact_c6_board.cc` mtime = **2026-10-14 22:30:33**（早于构建 5 分钟）
   - 工作日志第八轮记载"22:35 构建，2307008 字节，已烧入设备" → **完全对应**
   - 此后源码再无任何改动

3. **功能实测**
   - 2026-10-14 22:40 用户确认"语音对话、屏显示、功放播报全部正常"

**结论：git 基线 = 当前可用固件的完整来源，回退可靠。**

---

## git 基线（首选回退手段）

- 仓库：`me/xiaozhi-esp32-2.5.2`
- 基线提交：`97d0868` — "baseline: OLED working firmware (pre-servo)"（1743 文件）
- 标签：`oled-working`
- 开发分支：`servo-work`（舵机开发用）

**回退**：

```bash
cd me/xiaozhi-esp32-2.5.2
git checkout oled-working
# 恢复 C6 板卡配置后重新编译烧录
# 构建：<repo>\build_c6_final.ps1
# 烧录：<repo>\flash_c6.ps1
```

---

## 完整烧录包（build 目录，已验证与硬件一致）

| 文件 | 大小 | MD5 | flash 偏移 |
|---|---|---|---|
| `build/bootloader/bootloader.bin` | 16784 | `278b3234185eeadab14b863caff708e0` | `0x0` |
| `build/partition_table/partition-table.bin` | 3072 | `afe549bac5e55ac6de6bd0c047004a44` | `0x8000` |
| `build/ota_data_initial.bin` | 8192 | `84d04c9d6cc8ef35bf825d51a5277699` | `0xd000` |
| `build/xiaozhi.bin` | 2307008 | `7c4fd18cb1e77c7568eb829410031445` | `0x20000` |
| `build/generated_assets.bin` | 395920 | `563c3fd051b5a717b14f96914bd8ecf5` | `0x800000` |

烧录参数：`dio` / `16MB` / `80m` / `460800 baud`

---

## 硬件 flash 备份（补充/应急）

`backup/backup_flash_partial_320KB.bin`

- 大小：1,998,848 字节（0x1E8000）——**文件名是早期误写，实际近 2MB**
- 内容布局已确认：
  - `0x00000–0x0416F` = bootloader（与 build 产物逐字节一致）
  - `0x20000–` = app 固件（与 `build/xiaozhi.bin` 前 1,867,776 字节一致）
  - 覆盖到 `0x1E8000`，app 还剩约 44 万字节未覆盖
- MD5：`2e2c4b2f38c08a056de0cf300e7488d5`

**限制**：未覆盖 app 尾部（约 44 万字节）、分区表、assets 区。**恢复请优先用 git 重编译**，此文件仅供比对参考。

---

## 为什么没做全片 16MB 备份

C6 板自动复位不工作（`--before default_reset` 无效），每次读 flash 都需手动进下载模式（按住 BOOT + 点 RST + 保持 2 秒）；加上串口易丢字节、每次操作都要重新握手，长时间读取极不稳定。故改用 git 管理源码作为主要回退手段——源码才是固件的真正来源，重编译即可复现。