# Host runbook — run the Rokid Max driver end-to-end on Manjaro

## Option A (recommended): nix-shell, no install, no systemd

Works because Monado is built in **in-process mode** (the OpenXR loader
loads `libopenxr_monado.so` directly via `XR_RUNTIME_JSON`) - no
monado-service, no systemd, no `sudo ninja install`.

On the host (not in the container), from the shared workspace root:

```
nix-shell rokid-xr/monado-shell.nix
# inside the shell:
rokid-xr/run-helloxr.sh list     # Monado should appear as a runtime
rokid-xr/run-helloxr.sh          # glasses on -> hello_xr -G Vulkan
```

The script derives every path from its own location (the git checkout),
builds into `build-host/` if needed, and regenerates the runtime JSON,
so no `/workspace` hardcoding exists anywhere on the host side. If you
prefer manual steps, derive the root yourself:

```
ROOT=$(git -C rokid-xr/third_party/monado rev-parse --show-toplevel)
export XR_RUNTIME_JSON=$ROOT/build-host/openxr_monado-dev.json
```

Vulkan note: the script auto-exports VK_ICD_FILENAMES from the host's
/usr/share/vulkan/icd.d (nix-shell hides system ICDs via XDG_DATA_DIRS).
If Vulkan still fails to init, set VK_ICD_FILENAMES yourself to your
GPU's json.

Compositor note: the glasses' output isn't flagged non-desktop, so the
script forces a normal window (XRT_COMPOSITOR_FORCE_XCB/WAYLAND by
session type) - drag it to the glasses, or fullscreen with
XRT_COMPOSITOR_XCB_FULLSCREEN=1.

## Option B: native build + systemd service (full install)

Everything below runs on the **host** (not in the container). The patch
and this file live in the shared `/workspace/rokid-xr/`.

## 1. Install build deps

```
sudo pacman -S --needed git cmake ninja python3 base-devel \
  vulkan-headers vulkan-icd-loader vulkan-radeon glslang eigen cjson \
  libdrm libusb udev hidapi dbus wayland wayland-protocols \
  libx11 libxcb libxrandr opencv4 openxr
```

(vulkan-radeon/amdgpu/intel — pick your GPU's; `openxr` provides the
loader and `hello_xr`.)

## 2. Get Monado + apply our patch

```
mkdir -p ~/src && cd ~/src
git clone --depth 1 https://gitlab.freedesktop.org/monado/monado.git
cd monado
git apply /workspace/rokid-xr/rokid-max-monado.patch
```

(If `/workspace` isn't the host bind mount, copy the patch file over.)

## 3. Build

```
cmake -B build -G Ninja -DXRT_FEATURE_SERVICE=ON
ninja -C build
```

Configure output should show `DRIVER_ROKID: ON`. Build takes a few
minutes. Install:

```
sudo ninja -C build install
```

This installs `monado-service` and the OpenXR runtime JSON
(`/usr/share/openxr/1/openxr_monado.json` + active-runtime symlink in
`/etc/xdg/openxr/1/` on some distros — check step 4).

## 4. Verify the runtime is active

```
openxr_runtime_list
```

Expected: "Monado" listed as the **active** runtime. If it says another
runtime (e.g. SteamVR), either deactivate that one or point hello_xr at
Monado explicitly later via `XR_RUNTIME_JSON`.

## 5. Run hello_xr

Plug in the glasses (DP + USB), make sure they are your **active
display or a window you can move to them**, then:

```
# systemd user service (typical Monado install):
systemctl --user daemon-reload
systemctl --user start monado.service

# then:
hello_xr -G Vulkan
```

If hello_xr can't find the runtime:
`XR_RUNTIME_JSON=/usr/share/openxr/1/openxr_monado.json hello_xr -G Vulkan`

If the compositor picks the wrong monitor:
try `XRT_COMPOSITOR_FORCE_WINDOW=1 hello_xr -G Vulkan` and drag the
window to the glasses (or set the glasses as primary display).

## 6. What you should see

A virtual room (white hall with cubes) that **rotates with your head**:
turn right → the world turns left. Tilt → roll. Pitch → look up/down.
Gravity-locked, no drift while still.

## Troubleshooting

| Symptom | Fix |
|---|---|
| no /dev/hidraw for glasses | your existing `/etc/udev/rules.d/99-rokid.rules` covers it; replug |
| driver not claiming (check `journalctl --user -u monado` or hello_xr output for "Rokid") | `ROKID_LOG=DEBUG` env before starting monado-service |
| view turns the WRONG way (right turn → world turns right) | tell me — means gyro Y flip regressed |
| view rolls/pitches mirrored | tell me — mount rotation issue |
| compositor Vulkan errors | run on the host GPU directly; in a VM/console session it must be a real session |

## Running other OpenXR apps

`run-openxr.sh <app> [args]` sets up the same environment as
run-helloxr.sh (runtime JSON, spawn trims, Vulkan ICDs, XCB target) and
execs any OpenXR app:

    ROKID_SPAWN_PITCH=10 rokid-xr/run-openxr.sh xrgears

Candidates in nixpkgs: `xrgears` (Vulkan demo), `blender` (VR scene
inspection with a real horizon; enable in preferences), `wayvr`
(desktop-as-virtual-screen; may expect the Monado *service* - if it
does, run via `monado-service` + `XRT_FEATURE_SERVICE=ON` build instead).

### App test results

- xrgears: works (run inside monado-shell + `nix-shell -p xrgears`;
  bare `-p` shell misses the WSI-capable Vulkan loader stack).
- Blender VR: opens but VR session fails with repeated
  "EGL Error (0x3000)" — Blender creates its own GL context (EGL) while
  Monado's GL client path is GLX/X11. Try forcing X11 (unset
  WAYLAND_DISPLAY, XDG_SESSION_TYPE=x11, EGL_PLATFORM=x11) before
  blaming the driver.
