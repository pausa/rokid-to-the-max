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
- **M3 — Monado dev environment**: build Monado from source, run with
  dummy/null HMD, understand `xrt_device` lifecycle.
- **M4 — Driver skeleton**: `drv/rokid_max.c` — register device via
  xrt_prober (vid/pid 04d2:162f, interface 2), read 0x11 reports, feed
  m_imu_fusion, expose HMD with M2 numbers, SBS display.
- **M5 — Polish**: recenter gesture (double-tap via Psensor event?),
  proximity sleep integration, upstream PR.

## Layout (as it fills in)

- `imu_preview.py` — M0
- `calibration.md` — M1/M2 findings
- `drv/` — M4+ Monado driver C sources
