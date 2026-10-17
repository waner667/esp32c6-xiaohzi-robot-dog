import os, sys, glob

# 1. 找 build 下的 bin
proj = r"<repo>\me\xiaozhi-esp32-2.5.2"
bins = []
for root, dirs, files in os.walk(os.path.join(proj, "build")):
    for f in files:
        if f.endswith(".bin"):
            p = os.path.join(root, f)
            bins.append((p, os.path.getsize(p)))
bins.sort(key=lambda x: -x[1])
print("=== BUILD BINS ===")
for p, s in bins[:20]:
    print(f"{s:>12}  {p}")

# 2. 找项目里所有 .bin（含根目录）
print("=== ALL .bin in project ===")
for p in glob.glob(os.path.join(proj, "**", "*.bin"), recursive=True)[:20]:
    print(f"{os.path.getsize(p):>12}  {p}")

# 3. COM 口
try:
    from serial.tools.list_ports import comports
    print("=== COM PORTS ===")
    found = False
    for c in comports():
        found = True
        print(f"{c.device}  |  {c.description}  |  VID:PID={c.vid:04X}:{c.pid:04X}" if c.vid else f"{c.device}  |  {c.description}")
    if not found:
        print("no ports")
except ImportError as e:
    print("serial not installed:", e)
