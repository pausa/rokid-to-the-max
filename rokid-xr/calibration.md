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

## M5 addendum 3 — the "drift" was reference-frame initialization

Constant ~3 deg/s vertical world motion while still = NOT drift: it is
m_imu_3dof's gravity correction sweeping from its identity start
("device is level") to the wearer's actual head pitch, at its minimum
correction rate (pi/60 rad/s = 3 deg/s). Bias values were already
settled; pitch was converging toward the true head pitch (~+40 deg,
monitor-viewing posture) and would have stopped.

Fix: initialize i3dof.rot from the first accel sample (rotate
device-gravity vector onto world +Y) - the view now starts at the
wearer's actual pose. Also serves as recenter-on-startup.

## M5 addendum 4 — spawn facing

hello_xr content (cubes) spawns 180 deg away from the raw gravity-init
yaw choice. Fix: premultiply a 180 deg world-yaw onto the initial
orientation: rot = R_y(pi) * R_gravity. Tracking unchanged (yaw sign
verified independently); only the spawn facing changes.

## M5 addendum 5 — CORRECTED frame map (M1 had a mirror error)

The P5/P6 side-pose interpretation was mirror-ambiguous (subject could
swap left/right). All reliable data (right-turn -93 raw Y, look-up
-0.23 raw X, gravity-correction convergence behavior in hello_xr) now
gives:

IMU frame: X = RIGHT, Y = down, Z = back (toward eyes)

Correct transforms (device frame = right, up, back):
- accel: IDENTITY (see addendum 8)
- gyro:  IDENTITY (see addendum 8) [firmware note: 
                         effect after mount rotation = negate X and Z]

Evidence: with the old (mirrored-X) accel transform the gravity
correction could not converge - it precessed the orientation forever
(the "cubes drifting upward" symptom, fusion ending up believing the
device was upside down: up = -Y world). With the corrected transforms
every measurement is consistent.

Frame-convention debugging lesson: a single mirror error in an axis
map does not show up in gravity-locked static tests - it shows up as
non-converging correction dynamics in live use.

## M5 addendum 6 — gyro sign table (empirical, hello_xr)

Final: gyro transform = (+x, +y, -z) with accel (x, -y, z).
- yaw: raw Y = -93 deg for right turn -> +Y passes through (correct)
- pitch: live check in hello_xr: -X gave inverted pitch -> +X correct
  (the earlier "-0.23 raw X for look-up" capture was mislabeled)
- roll: -Z; verify left-ear-down = positive roll in-app (pending user)

## M5 addendum 7 — the drifting cubes: m_imu_3dof limit cycle (UPSTREAM BUG)

Symptom: cubes drift continuously (never freezable), fusion orientation
slowly flips (device up pointing world -Y mid-flip in debug logs).

Root cause in m_imu_3dof.c gravity_correction():
- error_angle is SET when tilt > max_tilt_error (0.01 rad = 0.57 deg -
  hyper-sensitive: motion residue triggers it)
- correction RUNS while error_angle > min_tilt_error, at a MINIMUM rate
  of pi/60 rad/s = 3 deg/s - even when the real tilt is smaller
=> guaranteed over-correction => re-trigger => perpetual limit cycle.
The two constants are used exactly backwards relative to their names.

Fix: set when tilt > min_tilt_error (2.9 deg, real tilt worth
correcting), run while error_angle > max_tilt_error (0.57 deg residual
tolerance - overshoot below re-trigger threshold = stable).

## M5 addendum 8 — FINAL frame map (identity!)

The live waccel/raw debug output settled it: raw accel worn level reads
(0, +9.8, -1.3) - the IMU frame IS the Monado device frame
(right, up, back), for both accel and gyro. No transform. Identity.

The entire mount-rotation saga traced to mis-executed/mis-labeled rest
poses in the M1 guided calibration (P3/P5 ambiguities). Lesson recorded:
guided human pose calibration is unreliable for axis SIGN determination;
trust live in-app motion tests with debug vector output instead.

All previous addendum transform conclusions are superseded by this one.

## M5 final — working configuration

- accel/gyro: identity (firmware delivers device-frame data)
- fusion: device-ts PLL-mapped to host clock (skew ~7%)
- gyro bias: stillness-gated EMA in driver (Monado has none)
- init: gravity-aligned orientation + 180 deg spawn yaw
- m_imu_3dof.c: threshold swap fix (min/max_tilt_error used backwards)
- verified in hello_xr: gravity-locked pitch/roll, stable bias
  (+0.0032,+0.0024,-0.0008), no systematic drift, yaw tracking correct

## M5 addendum 9 — recenter gesture hardening

Single taps false-triggered the double-tap detector (impact rebound
ringing counted as the second spike; walking/shaking spikes too).
Hardened: spikes only count when (a) gyro quiet (< 0.5 rad/s biased)
for 300 ms before, (b) 150 ms refractory between counted spikes,
(c) threshold raised to 22 m/s^2 (tunable via Monado debug UI).

## M5 final spawn heading

spawn_yaw = 0 (no premultiplied yaw). Empirically settled: with identity
frames, the fusion's natural device forward (-Z) faces hello_xr content
at startup; the legacy 180-deg premultiply put content behind. Recenter
resets heading to spawn_yaw, so double-tap always faces content.

## M5 addendum 10 — final recenter design (deferred recenter + spawn trims)

Bugs found and fixed in the recenter path:

1. **Tap-contaminated recenter** (cubes spawned tilted/elsewhere): the
   original recenter read the instantaneous accel at the tap instant,
   which was still full of impact impulse - it aligned the world to the
   tap itself, baking in a permanent ~50 deg tilt. Fix: recenter is
   deferred ~400 ms after the double-tap, executed once the head is
   quiet (250 ms motion gate, 2 s timeout fallback), and aligns to the
   fusion's own 300 ms world-accel filter average (tap-free).
2. **Recenter now also resets heading** to spawn_yaw (extract current
   yaw about world Y, rotate by the difference) so the gesture returns
   content to the spawn direction from any head yaw.
3. **Final detector tuning**: threshold 14 m/s^2 (taps measure
   ~22 m/s^2 peaks), motion gate 0.7 rad/s (~40 deg/s) / 250 ms,
   refractory 150 ms. Spike magnitudes + reject reasons logged at DEBUG.
4. **Spawn trims** (optical axis vs IMU mount offset): ROKID_SPAWN_YAW
   and ROKID_SPAWN_PITCH env vars (default 0; ~-10/+10 removed most of
   a ~10 deg left/down offset in hello_xr). Applied at init and recenter;
   pitch trim also compensates the recenter. Yaw has no gravity reference,
   so it is empirical by nature. Trim tuning abandoned as cosmetic -
   only affects the test scene; real apps place their own content.
5. spawn_yaw default = 0 (no premultiplied yaw). Verified: with the
   identity frame map, device -Z faces hello_xr content at startup.

Behavior verified in hello_xr: double-tap arms at ~22 m/s^2, deferred
recenter fires ~0.4 s later, waccel snaps to (0, +9.9, 0) and holds,
bias untouched, cubes return to the spawn direction.
