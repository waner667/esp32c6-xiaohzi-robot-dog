PULSE_MIN = 500
PULSE_MAX = 2500
PERIOD    = 20000
RES_BITS  = 20
FULL      = 1 << RES_BITS

def angle_to_duty(deg):
    deg = max(0.0, min(180.0, deg))
    span = PULSE_MAX - PULSE_MIN
    pulse_us = int(PULSE_MIN + span * deg / 180.0)
    return int(pulse_us * FULL / PERIOD)

print(f"full scale = 2^{RES_BITS} = {FULL}")
print(f"{'angle':>7} {'pulse_us':>9} {'duty':>9} {'back_to_us':>11}")
for d in (0, 10, 45, 90, 135, 170, 180):
    duty = angle_to_duty(d)
    back = duty * PERIOD / FULL
    print(f"{d:>7} {int(PULSE_MIN+(PULSE_MAX-PULSE_MIN)*d/180.0):>9} {duty:>9} {back:>11.2f}")

print()
print("期望: 0deg->26214, 90deg->78643, 170deg->124402")
ok = (angle_to_duty(0) == 26214 and angle_to_duty(90) == 78643)
print("MATCH" if ok else "MISMATCH")

print()
print("--- 对比旧 bug ---")
for d in (0, 90, 180):
    old_duty = int(PULSE_MIN + (PULSE_MAX - PULSE_MIN) * d / 180.0)
    print(f"  {d:>3}deg: old duty={old_duty:>5} -> actual {old_duty*PERIOD/FULL:>6.2f}us  (SG90 需要 >=500us)")