#!/usr/bin/env python3
"""m1_pose.py <pose_name> - sample 3s of accel/gyro at rest, save to /tmp/m1_poses.json"""
import json, math, statistics, struct, sys, time
import hid

dev = hid.Device(path=b"/dev/hidraw3")
time.sleep(0.2)
s = []
t0 = time.monotonic()
while time.monotonic() - t0 < 3:
    buf = dev.read(64, timeout=100)
    if not buf or buf[0] != 0x11 or len(buf) < 33:
        continue
    s.append((struct.unpack_from("<fff", buf, 9), struct.unpack_from("<fff", buf, 21)))
dev.close()

a = [statistics.mean(x[0][i] for x in s) for i in range(3)]
astd = [statistics.pstdev(x[0][i] for x in s) for i in range(3)]
g = [statistics.mean(x[1][i] for x in s) for i in range(3)]
gstd = [statistics.pstdev(x[1][i] for x in s) for i in range(3)]
mag = math.sqrt(sum(v*v for v in a))

data = {}
try:
    data = json.load(open("/tmp/m1_poses.json"))
except Exception:
    pass
data[sys.argv[1]] = {"a": a, "astd": astd, "g": g, "gstd": gstd, "mag": mag}
json.dump(data, open("/tmp/m1_poses.json", "w"), indent=1)

print("%s: a=(%+7.4f,%+7.4f,%+7.4f) |a|=%.4f max_std=%.4f  g=(%+.3f,%+.3f,%+.3f)" %
      (sys.argv[1], *a, mag, max(astd + gstd), *g))
