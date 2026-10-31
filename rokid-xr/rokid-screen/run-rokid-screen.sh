#!/bin/bash
# Dev launcher for rokid-screen (grid of run-helloxr.sh).
# Runs against the locally built Monado runtime, with optional spawn trims.
set -e
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
LOADER="$ROOT/third_party/openxr-sdk-source/build-hr/src/loader"
BIN="$HERE/build/rokid-screen"

[ -x "$BIN" ] || {
	echo "rokid-screen not built. Run:"
	echo "  nix-shell rokid-xr/monado-shell.nix --run 'cmake -S rokid-xr/rokid-screen -B rokid-xr/rokid-screen/build -G Ninja && cmake --build rokid-xr/rokid-screen/build'"
	exit 1
}

# runtime manifest (same template the AppImage uses)
JSON=/tmp/rokid-screen-openxr.$(id -u).json
sed "s|@RUNTIME_LIB@|$ROOT/third_party/monado/build-host/src/xrt/targets/openxr/libopenxr_monado.so|" \
	"$HERE/../../rokid-xr/ship/openxr_monado-dev.json" > "$JSON" 2>/dev/null ||
	[ -f "$JSON" ] || {
	echo "no runtime template at ship/openxr_monado-dev.json; build the AppImage once or set XR_RUNTIME_JSON yourself"
	exit 1
}

export XR_RUNTIME_JSON="$JSON"
export LD_LIBRARY_PATH="$LOADER:$LD_LIBRARY_PATH"
# compositor: force xcb unless caller chose otherwise
export XRT_COMPOSITOR_FORCE_XCB="${XRT_COMPOSITOR_FORCE_XCB:-1}"

# shellcheck disable=SC2068
exec "$BIN" "$@"
