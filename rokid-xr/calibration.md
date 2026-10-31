# M1 Calibration — Rokid Max IMU (LSM6DSR via HID report 0x11)

Measured 2024-xx, guided 6-pose rest test + 90° rotation test.

## Constants for the driver

| Quantity | Value |
|---|---|
| Timestamp | **nanoseconds**, u64 LE, offset 1 (after report id) — verified vs host clock |
| Accel units | **m/s²** (|a| ≈ 9.78–9.89 at rest) |
| Gyro units | **rad/s** (90° turn integrated to −93.3°) |
| Report rate | ~450 Hz (2.4 ms period nominal; occasional 2.0 ms) |
| Report layout | id 0x11, ts u64 @1, accel f32x3 @9, gyro f32x3 @21, all LE |

## Axis map (IMU frame, as delivered in HID)

```
+X = wearer's LEFT
+Y = DOWN
+Z = BACKWARD (through lens back, toward eyes)
```

Right-handed frame: X×Y=Z ✓. Equivalently the IMU frame is rotated 180°
about Z relative to the standard (right, up, back) XR frame:

```
R_mount = diag(-1, -1, 1)     # IMU -> XR frame
```

Sign checks from poses:
- P1 lenses-up: a_z = −9.86 (gravity −Z when Z=backward, lenses up) ✓
- P3 upright:   a_y = −8.99 (gravity −Y when Y=down, upright) ✓
- P5/P6 sides:  a_x = ±9.70 (X=left ↔ left-side-down +g) ✓
- 90° right turn: gyro ∫ = −93.3° about Y (right = −Y in this frame) ✓

Sanity: this also explains M0's "+77° roll at rest" — atan2(a_y, a_z)
with a_y≈+9.8 (gravity on +Y) ≈ 90°, not a mounting anomaly.

## Observed quality

- Rest noise (pstdev over 3 s): accel < 0.06 m/s², gyro < 0.001 rad/s
- |a| varies 9.75–9.89 across poses (~1.4% — includes pose tilt error)
- P2 (lenses-down) systematically reads a_y ≈ −1.7: nose-bridge props the
  frame ~10°, not a calibration defect
- Yaw drift (M0, gyro-only): 0.15°/30 s → bias correction in fusion
  should make it negligible

## Implications for the Monado driver (M4)

- Feed m_imu_fusion: accel in m/s² (it accepts that), gyro in rad/s,
  ts → seconds by 1e-9
- Apply R_mount before fusion (or negate x/y, keep z)
- Psensor gaps: drop samples when ts gap > 100 ms, re-init fusion bias
- 90°-turn convention: right turn = negative Y — matches right-handed
  frame, no extra sign flips needed after R_mount

## M2 — FOV and scale (visual tests, glasses-as-display)

Method: labeled marker grid (m2_pattern.html) fullscreen on glasses
display; credit-card angular reference (8.56 cm @ 40 cm = 12.2°).

| Result | Value |
|---|---|
| Vignetting | none — all markers visible per eye, both eyes |
| Usable panel | full 1920×1080 (SBS: 960×1080 per eye) |
| Half-panel (960 px) | ≈ 24.4° (2 credit cards @ 40 cm) |
| px/deg | ≈ 39.3 |
| hFOV | ≈ 49° (±10%: card-distance uncertainty) |
| vFOV | ≈ 28° |
| Diagonal | ≈ 56° |

Sanity: matches Rokid's advertised ~45-50° FOV. For the Monado driver:
render full 960×1080 per eye, fov = (49°, 28°), no distortion mesh
needed initially (user-visible edge quality confirmed at panel edges).

## M4 addendum — gyro Y firmware flip (driver-level)

Worn head-turn tests + gravity-lock behavior revealed the firmware
remaps GYRO axes independently of ACCEL (LSM6DSR ORIENT_CFG_G):

- accel (device frame): (-x, -y, z)   [mount rotation only]
- gyro  (device frame): (-x, +y, z)   [mount rotation + Y unflip]

Evidence:
- right turn integrates to NEGATIVE raw gyro Y (measured twice);
  the (left, down, back) right-handed frame requires positive
- without the unflip, right turns produce mirrored yaw and gravity
  correction fights it (pitch/roll contamination to +50 deg)
- with the unflip: right turn = negative yaw (Monado-correct), pitch
  and roll gravity-locked and stable

Also: f32x3 @33 in report 0x11 is NOT a magnetometer (static under
rotation) - likely temperature telemetry. Proxy bit @46 = Psensor.

## M5 addendum — final gyro transform (hello_xr empirical)

hello_xr revealed the earlier gyro-Y-only conclusion was incomplete:
pitch was inverted in-app (gravity-referenced pitch can't detect a gyro
sign error - only motion fidelity can). Full picture:

- firmware flips BOTH gyro X and Y (not Z) via LSM6DSR ORIENT_CFG_G
- net effect: gyro arrives ALREADY in device frame (right, up, back)
- final gyro transform: IDENTITY (raw passes through)
- accel transform stays: (-x, -y, z)

Evidence: yaw correct AND pitch correct with identity gyro transform;
pitch inverted when x was negated; yaw unconditionally correct because
both theories (flip-y-only, flip-xy) produce the same Y result.

Lesson: gravity-locking validates accel, but gyro signs need live
motion checks in a rendered scene (or deliberate fast-motion captures).

## M5 addendum 2 — drift mitigation

Symptom: in-app slow vertical world drift while still (pitch limit
cycle: gyro bias rotates, gravity correction pulls back).

Fix 1: fusion now uses DEVICE timestamps (ns, stable 450 Hz cadence)
mapped onto the host clock once at startup, instead of USB arrival
times - removes scheduling jitter from fusion dt, speeds up gyro bias
estimation convergence.

Note: bias estimation needs ~1 min of stillness to converge; some
residual slow drift right after startup is expected.
