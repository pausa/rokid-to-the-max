#!/usr/bin/env python3
"""imu_preview.py - M0 of rokid-xr: live orientation from Rokid Max IMU.

Reads HID report 0x11 (u64 ns timestamp, accel f32x3 @ 9, gyro f32x3 @ 21,
little-endian) from the glasses' HID interface and runs a complementary
filter -> pitch/roll/yaw, printed ~10 Hz.

Wear the glasses (Psensor gates the IMU). Press Ctrl-C to quit.
Recenter: hold glasses level, run once - current pose becomes zero.

Usage: imu_preview.py [--path /dev/hidrawN] [--hz 10]
Finds the device by vid/pid automatically if --path is omitted.
"""
import argparse
import math
import struct
import sys
import time

import hid  # hidapi

VID, PID = 0x04D2, 0x162F
REPORT_0x11 = 0x11


def find_path():
    for d in hid.enumerate(VID, PID):
        # interface 2 = IMU HID device; on hidraw each interface with a
        # report descriptor shows up - pick the one that streams 0x11
        if d["usage_page"] == 0xFF00 or d["usage"] == 0x0001:
            return d["path"]
    # fallback: any interface of the device; validate by data
    for d in hid.enumerate(VID, PID):
        return d["path"]
    return None


def looks_like_imu(buf):
    # 0x11 report: accel/gyro floats should be in plausible ranges
    ax, ay, az = struct.unpack_from("<fff", buf, 9)
    gx, gy, gz = struct.unpack_from("<fff", buf, 21)
    a_mag2 = ax * ax + ay * ay + az * az
    g_mag2 = gx * gx + gy * gy + gz * gz
    return 0.1 < a_mag2 < 100.0 and g_mag2 < 2000.0


class Complementary:
    """Simple complementary filter: high-pass gyro, low-pass accel."""

    def __init__(self, tau=0.5):
        self.tau = tau  # accel trust time constant, s
        self.pitch = 0.0
        self.roll = 0.0
        self.yaw = 0.0
        self.have_ref = False

    def step(self, accel, gyro, dt):
        ax, ay, az = accel
        gx, gy, gz = gyro

        # accel-derived pitch/roll (assuming units of g; yaw unobservable)
        ap = math.atan2(-ax, math.hypot(ay, az))
        ar = math.atan2(ay, az)

        if not self.have_ref:
            self.pitch, self.roll = ap, ar
            self.have_ref = True
        else:
            a = dt / (self.tau + dt)  # weight of accel
            self.pitch += a * (ap - self.pitch) + math.radians(gy) * dt
            self.roll += a * (ar - self.roll) - math.radians(gx) * dt
        # yaw = gyro integration only (no mag reference)
        self.yaw += math.radians(gz) * dt

    def recenter(self, accel):
        self.pitch = math.atan2(-accel[0], math.hypot(accel[1], accel[2]))
        self.roll = math.atan2(accel[1], accel[2])
        self.yaw = 0.0
        self.have_ref = True


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--path")
    p.add_argument("--hz", type=float, default=10)
    p.add_argument("--raw", action="store_true", help="print raw samples")
    args = p.parse_args()

    path = args.path or find_path()
    if not path:
        sys.exit("error: no Rokid Max HID interface found (plug in glasses)")
    dev = hid.Device(path=path)  # nix-shell hidapi API
    print(f"opened {path}")

    filt = Complementary()
    last_ts = None
    last_print = 0.0
    n = 0
    try:
        while True:
            buf = dev.read(64, timeout=100)  # timeout in ms (int)
            if not buf:
                continue
            if buf[0] != REPORT_0x11 or len(buf) < 33:
                continue
            if not looks_like_imu(buf):
                continue

            ts = struct.unpack_from("<Q", buf, 1)[0]
            accel = struct.unpack_from("<fff", buf, 9)
            gyro = struct.unpack_from("<fff", buf, 21)

            if last_ts is not None:
                dt = (ts - last_ts) / 1e9
                if 0 < dt < 0.1:  # ignore gaps (Psensor sleep etc.)
                    filt.step(accel, gyro, dt)
            last_ts = ts
            n += 1

            now = time.monotonic()
            if now - last_print >= 1.0 / args.hz:
                last_print = now
                if args.raw:
                    print(f"ts={ts} a=({accel[0]:+.3f},{accel[1]:+.3f},"
                          f"{accel[2]:+.3f}) g=({gyro[0]:+.2f},{gyro[1]:+.2f},"
                          f"{gyro[2]:+.2f}) rate={n}/win")
                else:
                    print(f"pitch {filt.pitch:+7.1f}°  roll {filt.roll:+7.1f}°  "
                          f"yaw {filt.yaw:+7.1f}°   ({n} samples since last)")
                n = 0
    except KeyboardInterrupt:
        pass
    finally:
        dev.close()


if __name__ == "__main__":
    main()
