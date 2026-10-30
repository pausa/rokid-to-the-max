# Rokid Max display-tweak lab setup

Goal: sniff + replay the control protocol the Rokid app uses to talk to the
glasses' internal MCU (brightness, color temp, 3D mode), then script custom
presets.

Hardware target: **plain Rokid Max** — USB ID `04d2:162f`.
Display stream = plain DisplayPort alt-mode; only the control channel is of
interest here.

---

## 1. Host setup (Manjaro, once)

usbmon runs in the host kernel — containers can't load it, so:

```bash
sudo modprobe usbmon
# only if debugfs is not mounted yet:
sudo mount -t debugfs none /sys/kernel/debug
```

Optional: make it persistent across reboots:

```bash
echo usbmon | sudo tee /etc/modules-load.d/usbmon.conf
```

## 2. Docker container with USB access

`lsusb` can list devices from sysfs alone, but real I/O (pyusb, hid) and
capture need device nodes + debugfs inside the container.

Start the container with:

```bash
docker run -it --rm \
  --privileged \
  -v /sys/kernel/debug:/sys/kernel/debug \
  -v /lib/modules:/lib/modules:ro \
  -v /workspace:/workspace \
  <your-image>
```

Least-privilege alternative to `--privileged`:

```bash
docker run -it --rm \
  --device=/dev/bus/usb \
  -v /sys/kernel/debug:/sys/kernel/debug \
  --cap-add=SYS_ADMIN \
  -v /lib/modules:/lib/modules:ro \
  <your-image>
```

Flag meanings:

| Flag | Why |
|---|---|
| `--device=/dev/bus/usb` | USB device nodes for pyusb/hid I/O |
| `-v /sys/kernel/debug` | usbmon sockets live here; tshark reads them |
| `--cap-add=SYS_ADMIN` | mount debugfs / open usbmon (skip if host already mounted) |
| `-v /lib/modules:ro` | lets container `modprobe usbmon` against host kernel |

## 3. Enter the nix shell

```bash
cd /workspace
nix-shell
```

`shell.nix` provides: `usbutils` (lsusb), `tshark`, Python with
`pyusb` / `hid` / `pyserial`, `kmod`.

shellHook prints an env check; ignore the warnings there, the real test is
step 4.

## 4. Verify inside the container

```bash
ls /dev/bus/usb/001/011     # glasses device node exists
ls /sys/kernel/debug/usbmon # capture interfaces exist
lsusb | grep -i rokid       # should show 04d2:162f
```

All three must pass before continuing. If the first two fail, the docker
flags / host modprobe step was skipped.

Note: bus/device numbers change on replug. Re-check with
`lsusb | grep -i rokid` and substitute the new numbers everywhere.

## 5. Capture the control protocol

Plug in the glasses, open the Rokid app on your phone (glasses connected via
the app), then start a capture filtered to the glasses' address:

```bash
# bus 1, device 11 (adjust to your lsusb output)
tshark -i usbmon1 -f "bus 1" -Y "usb.device_address == 11" -w rokid.pcapng
```

While capturing, in the app:
1. change brightness (note the order of steps)
2. change color temperature
3. toggle 3D / off-to-side mode

Stop capture (Ctrl-C). Inspect:

```bash
tshark -r rokid.pcapng -Y "usb.transfer_type == 0x02" -T fields \
  -e usb.src -e usb.dst -e usb.setup.bRequest -e usb.capdata
```

- `usb.transfer_type == 0x02` — control transfers
- also try `usb.transfer_type == 0x03` (interrupt / HID) — Rokid control
  channel may be HID vendor-defined
- set transfers host→glasses, get transfers back — match request/response pairs

Replay in Python (pyusb):

```python
import usb.core
dev = usb.core.find(idVendor=0x04d2, idProduct=0x162f)
dev.set_configuration()
# example: control transfer, adjust to captured values
dev.ctrl_transfer(0x21, bRequest, wValue, wIndex, payload)
```

## 6. Findings so far (updated as we probe)

Device composite layout (from `lsusb -v` + sysfs):

| Interface | Class | Role |
|---|---|---|
| 0 | Audio (control) | audio |
| 1 | Audio (streaming), 68B EP3 IN | audio |
| 2 | HID vendor page 0xff00, 64B EP2 IN/OUT | **control channel** |
| 3 | CDC-ACM (AT-command protocol byte) | serial — not yet probed |

### HID report descriptor (confirmed by reading sysfs)

- Report ID `0x01`: 63-byte **Output** — host → glasses commands
- Report IDs `0x02, 0x03, 0x04`: 63-byte **Input** streams, glasses → host

Input decode (confirmed against robert-akopyan/rokid-vr `rokid/hid.cpp`, a
native Windows SteamVR driver for exactly PID 0x162f):

- Reports start with ID `0x11` (17) or `0x04`.
- `0x11`: u64 timestamp, then accel (3 floats @ off 9), gyro (3 floats @ off 21).
- `0x04`: subtypes 1=accel, 2=gyro, 3=unknown, each 3 floats + ms timestamp.
- All sensor data little-endian float32.

### What this means for display tweaks

- **IMU / head tracking**: fully mapped, ready to use.
- **3D / SBS mode**: NOT an MCU command. rokid-vr never writes HID to the
  glasses; it renders side-by-side into the glasses' wide display mode from
  the GPU. So 3D = display-mode switching at the DP source.
- **Brightness / color temp**: not in the HID or the rokid-vr driver. Still
  open. Candidate channels:
  1. CDC-ACM serial (interface 3) — needs `/dev/ttyACM*` in the container
     (`--device=/dev/ttyACM0` or `--privileged`).
  2. Firmware dump of the MCU (escalation path below).

### Vendor control transfers: dead end (probed)

`probe_vendor.py` sent GET (`0xC1`) vendor requests, commands 0x00–0xF0,
on interfaces 0–2. All returned 64 zero bytes on every interface. That
command table is Max2-specific (PID 0x2002, see xelsed/rokidmaxwmdapi) —
not implemented on plain Max. Device survived.

### CDC-ACM serial: OPEN — full debug shell ("bayou")

`/dev/ttyACM0`, 115200 baud, 8N1. MCU streams register-access logs
(aw883xx audio amp state) — and accepts input. Sending `help\r\n` returns
a 28-command shell at prompt `bayou>`:

```
~          reset cpu (DANGER)          settime/gettime   time
md/mm      memory display/modify       ps                thread status
log        log level                   i2c-read/-writex/-writeb/-write/-scan
setpin/getpin/initpin                  gpio
reboot     reboot [rom|restore|factory|normal]  (DANGER)
crc        crc [addr] [len]            setpwm            setpwm [timer] [ccr]
gettemp    gettemp [spi_cs]            spi-write/spi-read
cs reg val / cs reg
lsmod      driver list                 version           firmware version
getenv/setenv/erase   env store (setenv/erase = persistent, DANGER)
backlight  set backlight level   <-- display tweak goal
```

Rules for this shell:
- Read-only first: `version`, `gettime`, `ps`, `lsmod`, `getenv`, `gettemp`,
  `i2c-read`, `spi-read`, `backlight` with no args (usage check).
- Never send: `~`, `reboot`, `mm`, `erase`, `i2c-write*`, `spi-write`,
  `setenv` until a full understanding of the mapping is established.
- **`i2c-scan` can hard-wedge the MCU** (confirmed: scanning an unknown bus
  blocked the DriverServer thread; IMU stream died, console I/O hung).
  Recovery: unplug/replug the glasses. Only use `i2c-scan` on known-good
  buses, if at all — prefer targeted `i2c-read` of known chip addresses.

### Display control: SOLVED (with caveats)

At log level 5, boot prints the map:

```
oled brightness 80 - 240          <- panel brightness range
spi [1] read reg 0x10, ...        <- panels are on SPI, chip-select 1/2
spi [2] read reg 0x10, ...
```

- **CS1 = `oled_left`, CS2 = `oled_right`** (confirmed by `spi-read`
  echoing the driver's own labels)
- **Brightness = SPI register `0x13` on each panel**. Confirmed live:
  `spi-write <cs> 0x13 0x80` to both eyes visibly dimmed the display;
  boot default was `0xf0` (240, top of the 80-240 range).
- Shell `backlight <n>` does NOT drive the panel (0/1/50/0xff: no effect).
- Color temp: driver init logs `oled color temp 0xf114` — panel SPI
  register not yet located.

**Caveat — readback inconsistency:** after a series of writes, register
0x13 read back 0x00 on both eyes even after re-writing 0xf0/0x80 (values
that verified correctly on the first pass). Visual state at that point:
unverified. Recovery for any dimmed/stuck state: replug — the driver
re-initializes panels to 0xf0 at boot (observed every boot).

**Caveat — proximity sensor (Psensor):** when the glasses are NOT being
worn, the display sleeps and panel SPI reads return `0x00` regardless of
written values. This mimics a broken readback and wasted a probing cycle.
RULE: only probe with the glasses worn; announce it and confirm before the
session. All reads/writes while unhooded are meaningless.

Lesson: write only with immediate readback verification, one eye at a
time, and treat any readback mismatch as stop-and-recheck (worn? replug?).

## Usage: color temperature control (working)

```
nix-shell --run './rokid-temp neutral'   # 0x14, stock tint (judged best)
nix-shell --run './rokid-temp warm'      # 0x08, slightly warm
nix-shell --run './rokid-temp cool'      # 0x20, slightly cool
nix-shell --run './rokid-temp 0x30'      # raw value
```

Panel SPI register 0x3c (with 0x3b held at 0xf1) is the color temp
control: higher = cooler (bluer), lower = warmer. Default 0x14 = the
boot log's `oled color temp 0xf114` (0x3b=0xf1, 0x3c=0x14). Verified via
side-by-side A/B (left eye warm / right eye cool): user judged neutral
most comfortable; extremes 0x00/0x40 work but look odd (likely clipped
gain). Same wear-check/verify discipline as rokid-brightness. Settings
do not persist across replug.

### Gamma: not panel-addressable (write-protected)

Full register sweep extended to 0xff. No LUT-style ramps anywhere.
The only tone-like structure is 0xf4-0xfd, a repeating [42 71 7e] x3
per-channel triplet - and it is WRITE-PROTECTED: spi-writes are silently
ignored (no echo, readback unchanged), unlike the brightness/colortemp
registers which echo and stick. Per-eye factory trim exists at 0x9d/0xa0
(values differ by 1 between eyes, also guarded). Gamma correction is
therefore GPU-side: xrandr --gamma, Wayland gamma-control, redshift, etc.
The glasses present as a normal DP display, so those all work.

### IPD: not firmware-addressable

Full panel register sweep 0x00-0x7f (both eyes, diffed): only 0x02 and
0x04 differ between eyes (orientation/scan flags - right panel is the
mirrored twin). No image-offset registers exist; IPD compensation can
only be done GPU-side (per-eye render shift), like the 3D/SBS path.
Panel geometry constants (e.g. 0x78 = 1920) appear near 0x58-0x5d.

### Firmware / hardware map (from shell)

- Firmware 0.23, PCBA 4, build Apr 27 2023
- Boot log: `oled color temp 0xf114` — color temp is a Sony OLED driver
  parameter (value 0xf114 at init) — the color-tweak target
- Drivers (`lsmod`): Lt7911ux (DP->MIPI bridge), Sony oled driver, Psensor,
  lsm6dsr (IMU, matches HID decode), gpio key, audio codec, GX8002 voice DSP
- `backlight` takes an argument (bare call = usage error, no syntax printed)


### Reference projects

- `robert-akopyan/rokid-vr` — plain Max (0x162f) HID decode, no HID writes
- `xelsed/rokidmaxwmdapi` — Max2 (0x2002) vendor protocol, control transfers
- `Anezium/awesome-rokid` — ecosystem list

## Usage: brightness control (working)

```
nix-shell --run './rokid-brightness desktop'   # 0x80, general use
nix-shell --run './rokid-brightness night'     # 0x20, dark rooms
nix-shell --run './rokid-brightness outdoor'   # 0xf0, max
nix-shell --run './rokid-brightness 0xa0'      # raw value
```

Calibrated presets (live-verified): desktop 0x80, night 0x20 (below the
driver's documented floor of 80 - works anyway), outdoor 0xf0.
The CLI does a wear-check first (panels read 0x00 when not worn), writes
both eye panels, and verifies readback. Settings do not persist across
replug (boot default is 0xf0).

## Escalation path (only if protocol isn't enough)

Dumping the MCU firmware: needs hardware (ST-Link V2 clone or SOIC8 clip) +
Ghidra + OpenOCD/flashrom. Brick risk, no schematics, no vendor docs.
Do not go here before exhausting the protocol path.

## Warnings

- Read-only sniffing is safe. **Writes to the control interface can change
  persisted MCU state or brick the glasses** — verify against a capture
  before replaying anything beyond known-good app commands.
- Don't send writes while a capture-based guess is unconfirmed. Start with
  replaying captured packets verbatim.
