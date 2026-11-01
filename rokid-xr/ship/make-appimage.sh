#!/usr/bin/env bash
# make-appimage.sh - build a portable x86_64 AppImage of the Rokid Max
# Monado runtime + grid-floor hello_xr. Run in the nix-capable container.
#
# Approach:
#   - collect the full runtime closure of our binaries (ldd + nix store
#     reference fixpoint, including the Mesa Vulkan ICD drivers which ldd
#     can't see)
#   - flatten all nix-store libs into AppDir/usr/lib, patchelf rpath
#     $ORIGIN onto everything
#   - keep the nix ELF interpreter, but launch via the bundled
#     ld-linux --library-path: the result is independent of the host
#     glibc (works on SteamOS's older glibc), while udev/dbus/X11 keep
#     talking to the host daemons as usual
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)   # rokid-xr/
OUT=${1:-"$HERE/../rokid-max-hmd.AppImage"}
# persistent AppDir: steps 1/2/4 (closure+flatten+patchelf) only run when
# the cache is empty or FORCE=1; artifacts are always refreshed, so FAST
# rebuilds (~20s) pick up recompiled binaries.
WORK=${WORK:-/tmp/rokid-appimg-cache}
APPDIR="$WORK/rokid-max-hmd.AppDir"
chmod -R u+w "$WORK.new" 2>/dev/null || true
rm -rf "$WORK.new"; mkdir -p "$WORK" "$WORK.new"
FULL=1
if [ -d "$APPDIR/usr/lib" ] && [ "${FORCE:-0}" != 1 ]; then
  FULL=0
  mv "$APPDIR" "$WORK.new/rokid-max-hmd.AppDir"
fi
APPDIR="$WORK.new/rokid-max-hmd.AppDir"
if [ "$FULL" = 0 ]; then echo "==> reusing cached AppDir (FORCE=1 for full rebuild)"; fi

echo "==> workdir $WORK"
mkdir -p "$APPDIR/usr/lib" "$APPDIR/usr/bin" \
         "$APPDIR/usr/share/vulkan/icd.d" "$APPDIR/usr/share/openxr"

MONADO="$HERE/third_party/monado/build-host"
HXSRCDIR="$HERE/third_party/openxr-sdk-source/build-hr"
RUNTIME_LIB="$MONADO/src/xrt/targets/openxr/libopenxr_monado.so"
HX="$HXSRCDIR/src/tests/hello_xr/hello_xr"
HXLOADER="$HXSRCDIR/src/loader/libopenxr_loader.so.1"

if [ "$FULL" = 1 ]; then
echo "==> 1/6 collecting runtime closure"
SEED_LIBS=(
  "$RUNTIME_LIB" "$HX" "$HXLOADER"
  /nix/store/lk4614327cn1h9z3m6cvnls95n13m1cj-vulkan-loader-1.4.357.0/lib/libvulkan.so.1
  /nix/store/v5c40hysv678q0yjwvdi0q97rdmxr9r4-mesa-26.2.1/lib/libvulkan_radeon.so
  /nix/store/v5c40hysv678q0yjwvdi0q97rdmxr9r4-mesa-26.2.1/lib/libvulkan_intel.so
  /nix/store/v5c40hysv678q0yjwvdi0q97rdmxr9r4-mesa-26.2.1/lib/libvulkan_lvp.so
)
: > /tmp/appimg-seeds.txt
for l in "${SEED_LIBS[@]}"; do
  ldd "$l" 2>/dev/null | grep -oE "/nix/store/[^ :]+" >> /tmp/appimg-seeds.txt || true
  echo "$l" >> /tmp/appimg-seeds.txt
done
sort -u /tmp/appimg-seeds.txt -o /tmp/appimg-seeds.txt

# fixpoint over direct references (catches deps invisible to ldd)
cp /tmp/appimg-seeds.txt /tmp/appimg-closure.txt
changed=1
while [ "$changed" = 1 ]; do
  changed=0
  while read -r p; do
    for d in $(nix-store -q --references "$p" 2>/dev/null); do
      if ! grep -qxF "$d" /tmp/appimg-closure.txt; then
        echo "$d" >> /tmp/appimg-closure.txt
        changed=1
      fi
    done
  done < /tmp/appimg-closure.txt
done
echo "    closure: $(wc -l < /tmp/appimg-closure.txt) store paths"

echo "==> 2/6 flattening libs into usr/lib"
i=0
while read -r p; do
  i=$((i+1))
  if [ -d "$p/lib" ]; then
    cp -aL "$p"/lib/*.so* "$APPDIR/usr/lib/" 2>/dev/null || true
  fi
done < /tmp/appimg-closure.txt
cp -aL /nix/store/n51dhmdbik1kfrsm62j5knavmigwrl1a-glibc-2.42-84/lib/ld-linux-x86-64.so.2 \
   "$APPDIR/usr/lib/" 2>/dev/null || true
# pulseaudio keeps its private lib in a subdir - the flatten above misses it
while read -r p; do
  [ -d "$p/lib/pulseaudio" ] && cp -aL "$p"/lib/pulseaudio/*.so* "$APPDIR/usr/lib/" 2>/dev/null || true
done < /tmp/appimg-closure.txt

fi  # FULL: closure + flatten

echo "==> 3/6 copying our artifacts"
cp "$RUNTIME_LIB" "$HX" "$HXLOADER" "$APPDIR/usr/lib/"
cp "$MONADO/src/xrt/targets/cli/monado-cli" "$APPDIR/usr/bin/" || true

# rokid-screen (desktop viewer) - drop its build-time rpath, the bundled
# ld invocation resolves libopenxr_loader.so from usr/lib
RS="$HERE/rokid-screen/build/rokid-screen"
if [ -x "$RS" ]; then
  cp "$RS" "$APPDIR/usr/lib/"
  [ -x /tmp/rokid-patchelf ] || nix-shell -p patchelf --run 'cp "$(command -v patchelf)" /tmp/rokid-patchelf'
/tmp/rokid-patchelf --remove-rpath "$APPDIR/usr/lib/rokid-screen"
  # copy any deps the closure missed (e.g. libwayland-client came in via
  # monado, but be safe)
  for dep in $(ldd "$RS" | awk '/=> \/nix\// {print $3}'); do
    base=$(basename "$dep")
    [ -e "$APPDIR/usr/lib/$base" ] || cp -L "$dep" "$APPDIR/usr/lib/"
  done
else
  echo "WARNING: rokid-screen not built - skipping (cmake -S rokid-screen -B rokid-screen/build)"
fi

# pipewire SPA plugins - dlopened at runtime, invisible to ldd
SPA_DIR=$(nix-shell "$HERE/monado-shell.nix" --run 'echo "$(pkg-config --variable=libdir libpipewire-0.3)/spa-0.2"' 2>/dev/null | tail -1)
if [ -d "$SPA_DIR" ]; then
  mkdir -p "$APPDIR/usr/lib/spa-0.2"
  cp -aL "$SPA_DIR"/. "$APPDIR/usr/lib/spa-0.2/"
  chmod -R u+w "$APPDIR/usr/lib/spa-0.2"  # nix store perms block later rm
else
  echo "WARNING: spa-0.2 plugins not found - pipewire capture will fail"
fi

if [ "$FULL" = 1 ]; then
echo "==> 4/6 patchelf rpath \$ORIGIN"
nix-shell -p patchelf --run '
  cd "'"$APPDIR"'/usr/lib"
  for f in *.so*; do
    [ -f "$f" ] && ! [ -L "$f" ] && patchelf --set-rpath "\$ORIGIN" "$f" 2>/dev/null || true
  done
  patchelf --set-rpath "\$ORIGIN" ../bin/* 2>/dev/null || true
'
fi

echo "==> 5/6 ICD JSONs (bare sonames) + OpenXR runtime JSON template"
for j in radeon_icd.x86_64.json intel_icd.x86_64.json lvp_icd.x86_64.json; do
  src="/nix/store/v5c40hysv678q0yjwvdi0q97rdmxr9r4-mesa-26.2.1/share/vulkan/icd.d/$j"
  sed 's|"library_path"[[:space:]]*:[[:space:]]*"[^"]*"|"library_path" : "libvulkan_PLACEHOLDER.so"|' "$src" > /dev/null # verify sed works
  sed "s|\"library_path\"[[:space:]]*:[[:space:]]*\"[^\"]*\"|\"library_path\" : \"$(basename "$j" | sed 's/_icd.x86_64.json//' | sed 's/radeon/libvulkan_radeon/;s/intel/libvulkan_intel/;s/lvp/libvulkan_lvp/').so\"|" "$src" > "$APPDIR/usr/share/vulkan/icd.d/$j"
done

cat > "$APPDIR/usr/share/openxr/openxr_monado-dev.json" << EOF
{
    "file_format_version" : "1.0.0",
    "runtime" : {
        "library_path" : "@RUNTIME_LIB@",
        "name" : "Monado (Rokid Max, portable bundle)"
    }
}
EOF

echo "==> 6/6 AppRun + desktop + meta"
cat > "$APPDIR/AppRun" << 'EOF'
#!/bin/bash
# AppRun - portable launcher for the Rokid Max Monado runtime.
#   rokid-max-hmd.AppImage                  # grid-floor hello_xr
#   rokid-max-hmd.AppImage <app> [args]     # app via bundled libs (nix-built apps)
#   rokid-max-hmd.AppImage --install-runtime [dir]   # stage runtime for host apps
#   rokid-max-hmd.AppImage --runtime-env    # print env for host apps
#   rokid-max-hmd.AppImage --install-udev   # print udev instructions
set -euo pipefail
APPDIR=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)

# NOTE: do NOT export LD_LIBRARY_PATH here! Host helper binaries run by
# this script (sed, id, ...) would pick up the bundled nix libc and die
# on GLIBC_PRIVATE mismatches ("only works inside nix-shell" bug - there
# PATH resolves helpers to nix coreutils). The bundled ld invocation
# below covers library resolution for the target binary instead.
export ROKID_SPAWN_YAW="${ROKID_SPAWN_YAW:-0}"
export ROKID_SPAWN_PITCH="${ROKID_SPAWN_PITCH:-0}"

# OpenXR runtime: generated copy with the absolute runtime lib path
# (${UID} is a bash builtin - do not run host 'id' here)
JSON="/tmp/rokid-max-openxr.${UID}.json"
sed "s|@RUNTIME_LIB@|$APPDIR/usr/lib/libopenxr_monado.so|" \
    "$APPDIR/usr/share/openxr/openxr_monado-dev.json" > "$JSON"
export XR_RUNTIME_JSON="$JSON"

# Bundled Mesa ICDs (bare sonames, found via LD_LIBRARY_PATH)
export VK_ICD_FILENAMES="$APPDIR/usr/share/vulkan/icd.d/radeon_icd.x86_64.json:$APPDIR/usr/share/vulkan/icd.d/intel_icd.x86_64.json:$APPDIR/usr/share/vulkan/icd.d/lvp_icd.x86_64.json"

# Force the X11(XCB) compositor target (no wayland target compiled in;
# XWayland serves X11 under Wayland sessions).
export XRT_COMPOSITOR_FORCE_XCB=1
export SPA_PLUGIN_DIR="$APPDIR/usr/lib/spa-0.2"

case "${1:-}" in
  --install-runtime)
    # Stage the OpenXR runtime (lib + deps + manifest) into a stable dir
    # so host OpenXR apps (blender, steam, ...) can use it directly.
    DEST="${2:-$HOME/.local/share/rokid-runtime}"
    mkdir -p "$DEST"
    cp -a "$APPDIR/usr/lib/". "$DEST/"
    sed "s|@RUNTIME_LIB@|$DEST/libopenxr_monado.so|" \
        "$APPDIR/usr/share/openxr/openxr_monado-dev.json" > "$DEST/openxr_monado-dev.json"
    echo "runtime installed: $DEST"
    echo "use with:  export XR_RUNTIME_JSON=$DEST/openxr_monado-dev.json"
    exit 0 ;;
  --runtime-env)
    # For host OpenXR apps: export ONLY the runtime manifest. The runtime
    # lib resolves its own deps via $ORIGIN rpath; do NOT export
    # LD_LIBRARY_PATH (host app + bundled libc = GLIBC_PRIVATE crash).
    DEST="$HOME/.local/share/rokid-runtime"
    if [ ! -f "$DEST/openxr_monado-dev.json" ]; then
      "$0" --install-runtime
    fi
    echo "export XR_RUNTIME_JSON=$DEST/openxr_monado-dev.json"
    exit 0 ;;
  --gdb)
    # Debug: run hello_xr under gdb with this environment intact.
    # Backtrace after the crash: bt
    exec gdb -q --args "$LD" --library-path "$APPDIR/usr/lib" \
        "$APPDIR/usr/lib/hello_xr" -G Vulkan "${@:2}" ;;
  --install-udev)
    echo "sudo cp $APPDIR/60-rokid-max.rules /etc/udev/rules.d/"
    echo "sudo udevadm control --reload"
    exit 0 ;;
esac

LD="$APPDIR/usr/lib/ld-linux-x86-64.so.2"
case "${1:-}" in
  ""|hello|helloxr)
    exec "$LD" --library-path "$APPDIR/usr/lib" "$APPDIR/usr/lib/hello_xr" -G Vulkan "${@:2}"
    ;;
  screen)
    exec "$LD" --library-path "$APPDIR/usr/lib" "$APPDIR/usr/lib/rokid-screen" "${@:2}"
    ;;
  --portal-test|--portal*)
    exec "$LD" --library-path "$APPDIR/usr/lib" "$APPDIR/usr/lib/rokid-screen" "$1" "${@:2}"
    ;;
  --gdb-screen)
    exec gdb -q --args "$LD" --library-path "$APPDIR/usr/lib" "$APPDIR/usr/lib/rokid-screen" "${@:2}"
    ;;
  list)
    exec "$LD" --library-path "$APPDIR/usr/lib" "$APPDIR/usr/lib/hello_xr" --help
    ;;
  *)
    APP="$1"; shift
    exec "$LD" --library-path "$APPDIR/usr/lib" "$APP" "$@"
    ;;
esac
EOF
chmod +x "$APPDIR/AppRun"

cat > "$APPDIR/rokid-max-hmd.desktop" << 'EOF'
[Desktop Entry]
Type=Application
Name=Rokid Max HMD (Monado)
Exec=AppRun
Icon=rokid-max-hmd
Categories=Development;
EOF
# minimal 1x1 png icon (placeholder)
python3 - << 'PYEOF'
import base64, zlib, struct
def chunk(t, d):
    c = t + d
    return struct.pack('>I', len(d)) + c + struct.pack('>I', zlib.crc32(c))
ihdr = struct.pack('>IIBBBBB', 128, 128, 8, 2, 0, 0, 0)
raw = b''.join(b'\x00' + b'\x40\x80\xc0' * 128 for _ in range(128))
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', ihdr) + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
open('rokid-max-hmd.png', 'wb').write(png)
PYEOF
mkdir -p "$APPDIR/usr/share/icons/hicolor/128x128/apps"
mv rokid-max-hmd.png "$APPDIR/usr/share/icons/hicolor/128x128/apps/"
cp "$APPDIR/usr/share/icons/hicolor/128x128/apps/rokid-max-hmd.png" "$APPDIR/.DirIcon"
cp "$HERE/ship/60-rokid-max.rules" "$APPDIR/"
cat > "$APPDIR/README.txt" << 'EOF'
Rokid Max as an OpenXR HMD - portable bundle (x86_64)

One-time setup (needs sudo, once per machine):
  ./rokid-max-hmd.AppImage --install-udev

Run:
  ./rokid-max-hmd.AppImage                 # hello_xr with grid floor
  ./rokid-max-hmd.AppImage <openxr-app>    # e.g. xrgears

Desktop screen in the glasses (wlr-screencopy, Hyprland/Sway/...):
  ./rokid-max-hmd.AppImage screen
  env: ROKID_SCREEN_FPS (30), ROKID_SCREEN_SCALE (1.0), ROKID_SCREEN_TEST=1

Use with host OpenXR apps (blender, steam, ...):
  eval "$(./rokid-max-hmd.AppImage --runtime-env)"
  # (stages the runtime into ~/.local/share/rokid-runtime once, then
  #  exports XR_RUNTIME_JSON for that shell)

Examples:
  blender                 # View > VR Session (host package)
  xrgears                 # nixpkgs OpenXR demo

Steam: add to the game's launch options:
  XR_RUNTIME_JSON=$HOME/.local/share/rokid-runtime/openxr_monado-dev.json %command%
  (native OpenXR titles and Proton titles with WineOpenXR; experimental)

Notes:
- double-tap the temple to recenter; spawns follow ROKID_SPAWN_YAW /
  ROKID_SPAWN_PITCH (degrees).
- the bundle ships its own glibc/mesa/vulkan; only udev, dbus and the
  X server come from the host.
EOF

echo "==> building AppImage"
cd "$WORK"
if [ ! -x "$WORK/squashfs-root/AppRun" ]; then
  curl -fsSL -o appimagetool.AppImage \
    "https://github.com/AppImage/appimagetool/releases/download/continuous/appimagetool-x86_64.AppImage"
  chmod +x appimagetool.AppImage
  ./appimagetool.AppImage --appimage-extract > /dev/null
fi
"$WORK/squashfs-root/AppRun" --no-appstream "$APPDIR" "$OUT"
# promote the freshly built AppDir to be the cache for next time
chmod -R u+w "$WORK.new" 2>/dev/null || true
rm -rf "$WORK"; mv "$WORK.new" "$WORK"
du -sh "$OUT"
echo "AppImage ready: $OUT"
