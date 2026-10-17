import subprocess, sys, os, time

ESPTOOL = r"<app>\ESP\Espressif\python_env\idf6.1_py3.13_env\Scripts\esptool.exe"
BUILD = r"<repo>\me\xiaozhi-esp32-2.5.2\build"
PORT = "COM10"
BAUD = "921600"

IMAGES = [
    ("0x0",      os.path.join(BUILD, "bootloader", "bootloader.bin")),
    ("0x8000",   os.path.join(BUILD, "partition_table", "partition-table.bin")),
    ("0xd000",   os.path.join(BUILD, "ota_data_initial.bin")),
    ("0x20000",  os.path.join(BUILD, "xiaozhi.bin")),
    ("0x800000", os.path.join(BUILD, "generated_assets.bin")),
]

# 单次连接：你手动进下载模式，--before no-reset 不再尝试复位，
# --after no-reset 烧完不复位（保持 stub 在线），随后单独 run。
def build_cmd():
    cmd = [ESPTOOL, "--chip", "esp32c6", "--port", PORT, "--baud", BAUD,
           "--before", "no-reset", "--after", "no-reset", "write-flash",
           "-z", "--flash-mode", "dio", "--flash-freq", "80m", "--flash-size", "16MB"]
    for off, path in IMAGES:
        cmd += [off, path]
    return cmd

deadline = time.time() + 120
attempt = 0
while time.time() < deadline:
    attempt += 1
    print(">>> attempt %d ..." % attempt, flush=True)
    r = subprocess.run(build_cmd(), capture_output=True, text=True)
    out = (r.stdout or "") + (r.stderr or "")
    # 连接失败 = 还没进下载模式，继续重试等待用户
    if "Failed to connect" in out or "No serial data received" in out or "Serial data stream stopped" in out:
        print("    not in download mode yet, waiting 3s ...", flush=True)
        time.sleep(3)
        continue
    print(out)
    print("FLASH rc=", r.returncode)
    if r.returncode == 0:
        sys.exit(0)
    print("    flash failed, retrying in 3s ...", flush=True)
    time.sleep(3)

print("TIMEOUT: never connected. Abort.")
sys.exit(2)