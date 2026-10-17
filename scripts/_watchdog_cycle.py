import serial, time, sys

PORT = "COM10"
BAUD = 115200
DURATION = 40

try:
    ser = serial.Serial(PORT, BAUD, timeout=1)
except Exception as e:
    print("open fail:", e)
    sys.exit(1)

print(f">>> reading {PORT} @ {BAUD} for {DURATION}s, watching DogLegCtrl/auto_cycle ...")
# 关键：DTR/RTS 拉低，避免开串口就把板子复位了（我们要看它自己跑）
ser.setDTR(False)
ser.setRTS(False)

kept = []
t0 = time.time()
while time.time() - t0 < DURATION:
    line = ser.readline()
    if not line:
        continue
    s = line.decode("utf-8", errors="replace").rstrip()
    low = s.lower()
    if ("dog" in low or "servo" in low or "mcp: add tool" in low
            or "brownout" in low or "guru" in low or "panic" in low
            or "rst:" in low or "watchdog" in low):
        kept.append(s)
        print(s, flush=True)

ser.close()
print(f"\n=== kept {len(kept)} interesting lines ===")
if not kept:
    print("!! NOTHING matched - board may be silent, crashed, or log level too low")