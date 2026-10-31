# Shipping the Rokid Max HMD stack

## Portable AppImage (the way)

`ship/make-appimage.sh` (run in the nix container) builds
`rokid-max-hmd.AppImage` (~230 MB, x86_64):

- libopenxr_monado.so runtime + grid-floor hello_xr + monado-cli
- the full nix-store runtime closure: own glibc 2.42, Mesa Vulkan
  (radeon/intel/lavapipe ICDs), vulkan-loader, libusb/hidapi/udev/dbus,
  X11/xcb libs — flattened into usr/lib with $ORIGIN rpaths
- launched via the bundled `ld-linux --library-path`, so it is
  independent of the host glibc (works on SteamOS 3.x which ships an
  older glibc); udev/dbus/X11 still talk to the host daemons

Target usage (no root, no nix, any distro):

    ./rokid-max-hmd.AppImage                    # hello_xr on the glasses
    ./rokid-max-hmd.AppImage xrgears            # any OpenXR app
    eval "$(./rokid-max-hmd.AppImage --runtime-env)"   # env for host apps
    ./rokid-max-hmd.AppImage --install-udev     # prints the sudo udev commands

Steam Deck (immutable): copy the AppImage anywhere in $HOME, install
the udev rule once (sudo works on SteamOS in desktop mode), run from
desktop mode. Desktop mode + XWayland = the XCB compositor target works.

## Superseded

`ship/make-bundle.sh` (nix-portable closure bundle) — abandoned:
nix-portable v010's DB registers paths on failed imports (validation
lies), proot fallback is fragile, and `nix-portable <cmd>` only runs
nix-distro binaries. Kept for reference only.

## Host apps (blender, steam, ...)

`--runtime-env` stages the runtime into ~/.local/share/rokid-runtime
(lib + deps via $ORIGIN rpath + manifest) and exports XR_RUNTIME_JSON
only - never LD_LIBRARY_PATH (host app + bundled libc = GLIBC_PRIVATE
crash). The app's own Vulkan loader + host ICDs drive the GPU.

    eval "$(./rokid-max-hmd.AppImage --runtime-env)"
    blender    # View > VR Session

Steam: game launch options
    XR_RUNTIME_JSON=$HOME/.local/share/rokid-runtime/openxr_monado-dev.json %command%
(native OpenXR and Proton/WineOpenXR titles; experimental)

glibc note: the runtime lib is built against glibc 2.42 - the HOST app
process must provide it (Manjaro-current OK; SteamOS 3.x is older, so
host apps there need a compat build against an older nixpkgs - TODO).
