import os, subprocess, sys

# 找 esptool
candidates = [
    r"<app>\ESP\Espressif\python_env\idf6.1_py3.13_env\Scripts\esptool.exe",
    r"<app>\ESP\Espressif\python_env\idf6.1_py3.13_env\Scripts\esptool.py",
]
for c in candidates:
    print(c, "EXISTS" if os.path.exists(c) else "missing")

# 也搜一下 anywhere under Espressif
print("=== search esptool ===")
for root, dirs, files in os.walk(r"<app>\ESP\Espressif\python_env"):
    for f in files:
        if f.startswith("esptool"):
            print(os.path.join(root, f))
    # 别走太深
    depth = root.count(os.sep) - r"<app>\ESP\Espressif\python_env".count(os.sep)
    if depth > 3:
        dirs[:] = []

# 看 partition table 内容（确认偏移）
proj = r"<repo>\me\xiaozhi-esp32-2.5.2"
pt = os.path.join(proj, "build", "partition_table", "partition-table.bin")
print("=== partition-table.bin first 512 bytes ===")
with open(pt, "rb") as f:
    data = f.read(512)
# partition table 是 csv-like，但二进制头部后跟条目；尝试解码可读部分
txt = data.decode("ascii", errors="replace")
print(txt[:400])

# 找 flash_args / flasher_args.json（里面写了每个分区的偏移）
fa = os.path.join(proj, "build", "flasher_args.json")
print("=== flasher_args.json exists:", os.path.exists(fa))
if os.path.exists(fa):
    import json
    with open(fa) as f:
        j = json.load(f)
    print("flash_files:", j.get("flash_files"))
    print("flash_settings:", j.get("flash_settings"))
    print("partition_table:", j.get("partition_table"))
