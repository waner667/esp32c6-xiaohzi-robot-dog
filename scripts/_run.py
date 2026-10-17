import subprocess, sys, time, os

ESPTOOL = r"<app>\ESP\Espressif\python_env\idf6.1_py3.13_env\Scripts\esptool.exe"
PORT = "COM10"

# 让停在 bootloader 的芯片 run（跳到正常固件）
print(">>> send run (jump to app)...")
r = subprocess.run([ESPTOOL, "--port", PORT, "run"], capture_output=True, text=True)
print(r.stdout, r.stderr)
print("run rc=", r.returncode)
