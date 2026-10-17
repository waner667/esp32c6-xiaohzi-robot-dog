import serial, subprocess, sys, os, time, threading

ESPTOOL = r"<app>\ESP\Espressif\python_env\idf6.1_py3.13_env\Scripts\esptool.exe"
PORT = "COM10"
OUT = r"<repo>\_serial_keys.log"
DURATION = float(sys.argv[1]) if len(sys.argv) > 1 else 40.0

buf = []
stop = threading.Event()

def reader():
    # dtr/rts 必须 False，否则开串口瞬间复位，开机日志全丢
    ser = serial.Serial(PORT, 115200, timeout=0.3, dsrdtr=False, rtscts=False)
    ser.dtr = False
    ser.rts = False
    t0 = time.time()
    while not stop.is_set():
        try:
            b = ser.read(512)
            if b:
                buf.append(b.decode("utf-8", "replace"))
        except Exception as e:
            buf.append("\n[read err] %r\n" % (e,))
    try:
        ser.close()
    except Exception:
        pass

th = threading.Thread(target=reader, daemon=True)
th.start()
time.sleep(0.4)

print(">>> send run (jump to app)...")
r = subprocess.run([ESPTOOL, "--port", PORT, "run"], capture_output=True, text=True)
print((r.stdout or "") + (r.stderr or ""), "run rc=", r.returncode)

time.sleep(DURATION)
stop.set()
th.join(timeout=2)

text = "".join(buf)
with open(OUT, "w", encoding="utf-8") as f:
    f.write(text)
print("=== captured %d chars -> %s ===" % (len(text), OUT))
print(text[-6000:])