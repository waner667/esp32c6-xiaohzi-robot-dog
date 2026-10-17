import subprocess, sys, os

ESPTOOL = r"<app>\ESP\Espressif\python_env\idf6.1_py3.13_env\Scripts\esptool.exe"
BUILD = r"<repo>\me\xiaozhi-esp32-2.5.2\build"
PORT = "COM10"
BAUD = "921600"

# 单次调用：你已经手动进下载模式，--before no-reset 不尝试复位，
# --after no-reset 烧完也不复位（保持 stub），由后面单独硬复位。
flash = [
    ESPTOOL,
    "--chip", "esp32c6",
    "--port", PORT,
    "--baud", BAUD,
    "--before", "no-reset",
    "--after", "no-reset",
    "write-flash",
    "-z",
    "--flash-mode", "dio",
    "--flash-freq", "80m",
    "--flash-size", "16MB",
    "0x0",      os.path.join(BUILD, "bootloader", "bootloader.bin"),
    "0x8000",   os.path.join(BUILD, "partition_table", "partition-table.bin"),
    "0xd000",   os.path.join(BUILD, "ota_data_initial.bin"),
    "0x20000",  os.path.join(BUILD, "xiaozhi.bin"),
    "0x800000", os.path.join(BUILD, "generated_assets.bin"),
]
print(">>> flashing (single connection)...")
r = subprocess.run(flash, capture_output=True, text=True)
print(r.stdout)
print(r.stderr, file=sys.stderr)
print("FLASH rc=", r.returncode)
sys.exit(r.returncode)
