# rokid-xr — Rokid Max as a real HMD (Monado driver)

Long-term project: turn the plain Rokid Max (04d2:162f) into a first-class
OpenXR HMD under Linux via [Monado](https://gitlab.freedesktop.org/monado/monado/).
Everything we reverse-engineered on the display side is the foundation;
this project adds head-tracking, proper IPD, and OpenXR to any app.

## What we already have (from the parent project)

| Fact | Source |
|---|---|
| Display = plain 1920x1080@60 DP output; VR content = SBS into it | display probing |
| IMU = LSM6DSR on HID interface 2, `/dev/hidraw*` | HID decode |
| IMU report `0x11`: u64 ns timestamp, accel x/y/z float32 LE @ off 9, gyro @ off 21 | HID decode |
| IMU streams ~450 Hz while glasses worn (Psensor gate!) | live measurement |
| Panel register map (brightness 0x13, color temp 0x3b/0x3c) | this repo's tools |
| IPD is fixed in hardware; software must shift per-eye ±N px | full register sweep |

## Architecture

```
/dev/hidrawN ──> rokid_imu_driver.c ──> Monado xrt_device ──> OpenXR apps
   (LSM6DSR)         hid read +            imu fusion          (StereoKit,
                     timestamp             + SBS display        h264dec, etc.)
                     decode                + FOV/distortion
                                           + eye offsets (IPD)
```

Key Monado building blocks to use (don't reinvent):
- `xrt_device` driver interface + `xrt_prober` HID autodetection
- `m_imu_fusion` (Madgwick/Mahony fusion, C, already in Monado)
- Display: `xrt_device` HMD fields — fov, distortion mesh (or none),
  `views[2]` with per-view `rotation`/`pose`. For SBS-on-one-display,
  mirror/extend via normal DRM display.

## Unknowns / risks

| Unknown | Plan |
|---|---|
| FOV (lens + panel geometry) | empirical: fixed-pattern on screen, estimate edge angles |
| Lens distortion | probably negligible (Rokid uses weak aspheric optics); verify visually, start with rectilinear |
| IPD offset in px | subjective calibration slider (start 0, ±40 px range) |
| IMU mounting orientation / axes sign | calibrate: rotate on each axis, compare against gravity |
| Timestamp units (ns? 0.1us?) | cross-check against host clock drift |
| Psensor gating: IMU may stop when not worn | driver must handle stream gaps |

## Milestones

- **M0 — IMU validation (Python)**: `imu_preview.py` — live pitch/roll/yaw
  from raw 0x11 reports with a complementary filter + recenter.
  **DONE.** 447 Hz sustained, <1° wobble held still, yaw drift 0.15°/30 s.
  Open question for M1: IMU mounting frame is rotated vs assumed axes
  (reads ~+77° roll at rest) - recentering hides it; 6-orientation
  calibration will resolve the exact rotation.
- **M1 — Timestamp + axes calibration**: DONE — see `calibration.md`.
  ts = ns, accel = m/s², gyro = rad/s; IMU frame = (left, down, back),
  R_mount = diag(−1,−1,1); 90°-turn verified (−93.3°); the M0 "77° roll"
  was just the axis convention, not an anomaly.
- **M2 — FOV measurement**: DONE — marker-grid + credit-card method:
  no vignetting (full panel usable), hFOV ≈ 49°, vFOV ≈ 28°,
  ~39 px/deg. See `calibration.md`.
- **M3 — Monado dev environment**: DONE — `monado-shell.nix` (cmake,
  ninja, vulkan headers, glslang, libdrm, hidapi...); full build OK
  (~590 targets); `monado-cli` runs; prober sees the Max via libusb
  (04d2:162f, /dev/ttyACM0 listed as serial path).
  **Key discovery:** upstream already ships `src/xrt/drivers/rokid/` —
  but it is for the old Rokid **HDK** devkit and claims our Max by
  VID/PID collision. It reads endpoint 0x82 (iface 2 = our IMU) with the
  wrong packet format and issues vendor control transfers (GET 0x81 /
  SET 0x01). All reads return zeros on the Max and the SBS-mode write is
  silently ignored (consistent with our earlier control-transfer probe).
- **M4 — Driver**: DONE (first working state) — `drv_rokid` reworked:
  * mount rotation (accel: (-x,-y,z), gyro: (-x,+y,z) - firmware remaps
    gyro Y independently via LSM6DSR ORIENT_CFG_G; verified empirically:
    right turn = negative yaw, gravity lock on pitch/roll)
  * FOV per calibration.md: 49x28 deg (was 46v/82h HDK guesswork)
  * display-mode vendor writes skipped on Max (HDK-era command, ignored
    by Max MCU)
  * USB cleanup on destroy (release + kernel reattach - no more stale
    hidraw after driver exit)
  * periodic pose debug print at ROKID_LOG=DEBUG (HMD-frame euler)
  Remaining for M5: remove 8s debug hold, end-to-end OpenXR app test
  (hello_xr needs compositor + OpenXR loader), upstream patch.
- **M5 — end-to-end**: hello_xr RUNS on the glasses with 3DOF tracking:
  gravity-locked pitch/roll, stable bias, no drift (PLL-synced device
  clock), all axes verified. DONE: recenter gesture (double-tap,
  deferred + tap-filtered, resets heading to spawn) + u_var button;
  spawn trims via ROKID_SPAWN_YAW/_PITCH env (optical-vs-IMU offset,
  ~10 deg, cosmetic — only matters for the test scene). Roll verified
  (grid-floor hello_xr build: ear-tilt keeps the horizon horizontal).
  Remaining for upstreaming: remove 8s debug hold in pose print. Upstream-worthy
  findings: m_imu_3dof threshold swap (limit cycle) + missing gyro bias
  estimation. Upstream PR: pending.

## Layout (as it fills in)

- `imu_preview.py` — M0
- `m1_pose.py` — M1 pose sampler
- `m2_pattern.html` — M2 FOV test pattern
- `monado-shell.nix` — build env (incl. nix Mesa vulkan ICDs)
- `rokid-max-monado.patch` — the driver patch (regenerate: `git diff > patch`)
- `run-helloxr.sh` — launch hello_xr with in-process runtime (host+container path-agnostic)
- `update-driver.sh` — revert/re-apply patch + rebuild build-host (host use)
- `calibration.md` — M1/M2 constants + frame-convention findings
- `HOST_RUNBOOK.md` — host setup guide
- `drv/` — M4+ Monado driver C sources (actually: upstream `third_party/monado/src/xrt/drivers/rokid/`)
