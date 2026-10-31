#!/usr/bin/env bash
# run-openxr.sh - run ANY OpenXR app on the Rokid Max via the in-process
# Monado runtime. Sets up everything run-helloxr.sh does (runtime JSON,
# spawn trims, Vulkan ICDs, XCB compositor target) and then execs the
# given command.
#
# Usage:
#   ./run-openxr.sh <app> [args...]
#   XR_RUNTIME_JSON is exported automatically; app-specific env passes
#   through as usual, e.g.:
#     ROKID_SPAWN_PITCH=10 ./run-openxr.sh xrgears
#
# Tried-and-tested apps:
#   blender           # File > OpenXR Action Hero / VR inspection (has horizon!)
#   xrgears           # Vulkan OpenXR sanity demo (nixpkgs)
#   wayvr             # your desktop as a head-tracked virtual screen
#                     # (wlx-overlay-s successor; see notes below)

set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
MONADO="$HERE/third_party/monado"
BUILD="$MONADO/build-host"
LIB="$BUILD/src/xrt/targets/openxr/libopenxr_monado.so"
JSON="$BUILD/openxr_monado-dev.json"

[ -f "$LIB" ] && [ -f "$JSON" ] || {
	echo "Monado runtime not built - run ./run-helloxr.sh once (it builds it)" >&2
	exit 1
}

export XR_RUNTIME_JSON="$JSON"
export ROKID_SPAWN_YAW="${ROKID_SPAWN_YAW:-0}"
export ROKID_SPAWN_PITCH="${ROKID_SPAWN_PITCH:-0}"

# Vulkan ICDs (same logic as run-helloxr.sh): resolve bare sonames in
# host ICD JSONs to absolute paths so the nix ld.so can find them.
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
				*lvp*|*swrast*|*softpipe*) continue ;;
				*) found="$found$ICD_OUT/$(basename "$f"):" ;;
			esac
		done
	done
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

# Same compositor constraint as hello_xr: no wayland target compiled in,
# force the X11(XCB) window backend (XWayland serves it).
export XRT_COMPOSITOR_FORCE_XCB="${XRT_COMPOSITOR_FORCE_XCB:-1}"

[ "${ROKIDX_TRACE:-0}" = 1 ] && echo "trace: XR_RUNTIME_JSON=$XR_RUNTIME_JSON VK_ICD_FILENAMES=${VK_ICD_FILENAMES:-<unset>}" >&2

exec "$@"
