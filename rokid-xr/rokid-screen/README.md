# rokid-screen — desktop screen in the Rokid Max

Head-locked desktop viewer: captures a Wayland output via
**wlr-screencopy** (Hyprland, Sway, river, ...) and copies it into both
eyes of an OpenXR swapchain served by the bundled Monado runtime.

Design: **no shaders, no mirror window, no overlay stack**. The desktop
pixels are transferred straight into the swapchain image
(`vkCmdCopyBufferToImage`, letterboxed black bars for aspect mismatch).
The image is anchored to the LOCAL reference space, so the driver's
double-tap recenter gesture moves the screen with the world.

## Build (inside `nix-shell rokid-xr/monado-shell.nix`)

```bash
cmake -S rokid-xr/rokid-screen -B rokid-xr/rokid-screen/build -G Ninja
cmake --build rokid-xr/rokid-screen/build
```

## Run

```bash
# dev flow (local runtime):
nix-shell rokid-xr/monado-shell.nix --run 'rokid-xr/rokid-screen/run-rokid-screen.sh'

# packaged flow (host, glasses plugged in):
./rokid-max-hmd.AppImage screen
```

## Env

| var                  | default | meaning                                    |
|----------------------|---------|--------------------------------------------|
| `ROKID_SCREEN_FPS`   | 30      | capture rate                               |
| `ROKID_SCREEN_SCALE` | 1.0     | fill factor (1.0 fills the whole FOV)      |
| `ROKID_SCREEN_TEST`  | 0       | animated test pattern, no Wayland needed   |
| `ROKID_SPAWN_YAW/PITCH` | 0    | optical-vs-IMU trims (set by AppRun)       |

## Notes

- Session type `HEAD_MOUNTED`, stereo; both eyes receive the same image
  (zero disparity — reads like a real monitor at arm's length).
- Swapchain usage: `COLOR_ATTACHMENT | TRANSFER_DST`; acquire/release
  layout contract (`COLOR_ATTACHMENT_OPTIMAL` ↔ `TRANSFER_DST_OPTIMAL`)
  follows Monado's `comp_vk_client.c` barriers.
- If no Wayland compositor is reachable, a test pattern is rendered
  instead (also used to validate the XR path in containers).
- X11-only sessions are not supported yet (wlr-screencopy is Wayland);
  use the PipeWire portal path or run a compositor with real Xwayland.

## Status (session log)

- **Working end-to-end**: session lifecycle (READY→SYNCHRONIZED→VISIBLE→
  FOCUSED via xrEndFrame's do_synchronize_state_change!), swapchains
  (2688x1512/eye on the real glasses), layer submission, render-pass
  pipeline, display. Test pattern verified **visible in the glasses**.
- **Key Monado facts learned**: xrWaitFrame blocks until the previous
  xrBeginFrame; session state only advances on xrEndFrame with layers;
  client swapchain images arrive in COLOR_ATTACHMENT_OPTIMAL and must
  return there before release.
- **Bugs fixed along the way**: swapchain image index cycling, layers
  array-of-pointers (SIGSEGV in oxr_session_frame_end), wlr-screencopy
  `buffer` event arg order (format FIRST: format,w,h,stride), output
  enumeration with stable listener addresses, shader embedding.
- **WORKING (checkpoint)**: niri wlr-screencopy capture -> glasses.
  Root causes found by reading niri/smithay sources: (1) niri needs a
  grim-style exact pool (stride*w bytes, buffer at offset 0, XRGB8888);
  (2) R/B swap needed with the .bgr shader (default on,
  ROKID_SCREEN_SWAP_RB=0 to restore); (3) sRGB swapchain format 43
  default - UNORM gets gamma-reencoded by Monado comp (washed out).
- PipeWire portal path also works end-to-end (frames reach glasses) but
  niri's xdg-desktop-portal-gnome serves the CAMERA node (YUY2 400x480,
  source_type mislabeled) - kept as ROKID_SCREEN_SOURCE=portal fallback
  for GNOME; YUY2 conversion included. Portal lessons: session_handle
  is a positional arg; session_handle_token REQUIRED (older xdg-p
  asserts without it); subscribe Response BEFORE calling.
- **Still open**: real wlr-screencopy capture rejected by Hyprland with
  `invalid buffer` even with correct event parsing, correct format from
  the buffer event, and roundtrip/flush before copy. grim works, so the
  protocol CAN work - something structural differs vs grim. Debug trail:
  logged frame params, wl_shm bound at advertised version.
- **Next step (agreed pivot)**: PipeWire portal capture
  (xdg-desktop-portal-hyprland -> pw_stream, BGRx). Compositor-blessed,
  works on GNOME/KDE too. Then: ROKID_SCREEN_SCALE tuning, wayvr revisited.
