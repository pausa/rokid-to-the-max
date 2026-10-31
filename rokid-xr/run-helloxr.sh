#!/usr/bin/env bash
# run-helloxr.sh - launch hello_xr with the in-process Monado runtime.
#
# All paths are derived from this script's own location (the git
# checkout), so it works identically inside the container (/workspace)
# and on any host bind-mount path. Uses build-host/ (fresh CMake dir) so
# a build tree created under a different mount point can't poison it.
#
# Usage:
#   ./run-helloxr.sh list        # openxr_runtime_list (runtime visible?)
#   ./run-helloxr.sh [args...]   # hello_xr -G Vulkan [args...]
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
MONADO="$HERE/third_party/monado"
BUILD="$MONADO/build-host"
LIB="$BUILD/src/xrt/targets/openxr/libopenxr_monado.so"
JSON="$BUILD/openxr_monado-dev.json"

need_build=0
[ -f "$LIB" ] || need_build=1
[ -f "$JSON" ] || need_build=1
# stale JSON pointing at another mount point (e.g. container /workspace)?
[ "$need_build" = 0 ] && grep -q "$LIB" "$JSON" || need_build=1

if [ "$need_build" = 1 ]; then
	echo "==> building Monado (in-process runtime + cli) in $BUILD"
	nix-shell "$HERE/monado-shell.nix" --run "
		cmake -S '$MONADO' -B '$BUILD' -G Ninja -DXRT_FEATURE_SERVICE=OFF &&
		ninja -C '$BUILD' monado-cli libopenxr_monado.so"
fi

export XR_RUNTIME_JSON="$JSON"

# Spawn trim (degrees): content placement vs the IMU/optical mount.
# Set e.g. ROKID_SPAWN_YAW=-10 ROKID_SPAWN_PITCH=10 ./run-helloxr.sh
# (driver defaults: both 0; applied at startup and on double-tap recenter)
export ROKID_SPAWN_YAW="${ROKID_SPAWN_YAW:-0}"
export ROKID_SPAWN_PITCH="${ROKID_SPAWN_PITCH:-0}"

# Vulkan ICDs, in order of preference:
# 1. VK_ICD_FILENAMES already set (or nix-shell shellHook set it to the
#    nix Mesa driver stack - self-contained, like nixGL).
# 2. Host ICDs patched to absolute paths (host Mesa via ldconfig).
# No pipelines in this block: set -euo pipefail turns a missing dir or
# non-matching grep into a silent exit-1.
if [ -z "${VK_ICD_FILENAMES:-}" ]; then
	ICD_OUT=$(mktemp -d /tmp/rokid-icds.XXXXXX)
	found=""
	for d in /usr/share/vulkan/icd.d /etc/vulkan/icd.d; do
		[ -d "$d" ] || continue
		for f in "$d"/*_icd*.json; do
			[ -f "$f" ] || continue
			lib=$(sed -n 's/.*"library_path"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$f")
			[ -n "$lib" ] || continue
			case "$lib" in
				/*) real="$lib" ;;
				*)
					real=$( (ldconfig -p 2>/dev/null || true) | awk -v l="$lib" '$1==l {print $4; exit}' )
					if [ -z "$real" ]; then
						for p in /usr/lib/$lib /usr/lib64/$lib /lib/$lib; do
							[ -f "$p" ] && real="$p" && break
						done
					fi
					;;
			esac
			if [ -z "$real" ] || [ ! -f "$real" ]; then
				echo "warning: could not resolve ICD driver for $f (skipping)" >&2
				continue
			fi
			sed "s|\"library_path\"[[:space:]]*:[[:space:]]*\"$lib\"|\"library_path\" : \"$real\"|" "$f" > "$ICD_OUT/$(basename "$f")"
			case "$f" in
				*lvp*|*swrast*|*softpipe*) continue ;;   # keep only as last resort
				*) found="$found$ICD_OUT/$(basename "$f"):" ;;
			esac
		done
	done
	# software renderers as fallback only
	if [ -z "$found" ]; then
		for f in "$ICD_OUT"/*.json; do
			[ -f "$f" ] || continue
			found="$found$f:"
		done
	fi
	if [ -n "$found" ]; then
		export VK_ICD_FILENAMES="${found%:}"
	fi
fi

[ "${ROKIDX_TRACE:-0}" = 1 ] && echo "trace: VK_ICD_FILENAMES=$VK_ICD_FILENAMES" >&2

# The glasses' output is not RandR-flagged non-desktop, so Monado's
# direct mode can't pick it ("No non-desktop output available").
# Force the X11(XCB) windowed target: our Monado build has no 'wayland'
# target compiled in, and under a Wayland session XWayland serves X11
# windows anyway. (Fullscreen: XRT_COMPOSITOR_XCB_FULLSCREEN=1.)
if [ -z "${XRT_COMPOSITOR_FORCE_XCB:-}" ]; then
	export XRT_COMPOSITOR_FORCE_XCB=1
fi

if [ "${1:-}" = "list" ]; then
	exec openxr_runtime_list
fi

exec hello_xr -G Vulkan "$@"
