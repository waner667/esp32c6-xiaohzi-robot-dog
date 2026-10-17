import serial, time, sys

PORT = "COM10"
BAUD = 115200

try:
    ser = serial.Serial(PORT, BAUD, timeout=1)
except Exception as e:
    print("open fail:", e)
    sys.exit(1)

print(f">>> reading {PORT} @ {BAUD} for 20s (after power-on) ...")
ser.setDTR(False)
ser.setRTS(False)
buf = []
t0 = time.time()
while time.time() - t0 < 20:
    line = ser.readline()
    if not line:
        continue
    try:
        s = line.decode("utf-8", errors="replace")
    except:
        s = repr(line)
    buf.append(s)
    sys.stdout.write(s)
    sys.stdout.flush()

ser.close()
print(f"\n=== captured {len(buf)} lines ===")
joined = "".join(buf).lower()
for kw in ["dog", "servo", "brownout", "guru", "panic", "assert", "wifi", "error",
          "fail", "timeout", "reset", "rst:", "rf", "voltage", "power"]:
    if kw in joined:
        print(f"[hit] {kw}")
