import serial, time, sys

PORT = 'COM10'
KW = ['DogLeg', 'auto_cycle', 'auto-cycle', 'MCP: Add tool', 'Application',
      'WifiBoard', 'Guru', 'panic', 'abort', 'Brownout', 'abort() was called']

ser = serial.Serial()
ser.port = PORT
ser.baudrate = 115200
ser.timeout = 0
# 关键：不要让开串口的动作复位板子
ser.dtr = False
ser.rts = False
ser.open()

t0 = time.time()
buf = bytearray()
while time.time() - t0 < 22:
    try:
        data = ser.read(4096)
        if data:
            buf.extend(data)
    except Exception as e:
        pass

ser.close()
text = buf.decode('utf-8', errors='replace')
lines = [l for l in text.splitlines() if l.strip()]
print('=== total lines:', len(lines), '===')
print('--- key lines ---')
for l in lines:
    if any(k in l for k in KW):
        print(l)
print('--- auto_cycle present? ---')
print('YES' if 'auto_cycle' in text or 'auto-cycle' in text else 'NO (correct, disabled)')
print('--- last 12 lines ---')
for l in lines[-12:]:
    print(l)
