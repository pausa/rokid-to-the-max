#!/usr/bin/env bash
# update-driver.sh - re-apply the Rokid Max patch and rebuild the
# in-process Monado runtime (build-host/), ready for run-helloxr.sh.
# Run it inside the nix-shell (or standalone - cmake/ninja are needed).
#
# Usage: ./update-driver.sh          # from anywhere
set -euo pipefail

HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
MONADO="$HERE/third_party/monado"
PATCH="$HERE/rokid-max-monado.patch"
BUILD="$MONADO/build-host"

cd "$MONADO"

# revert any previous patch application (idempotent)
if [ -n "$(git status --porcelain src/xrt/drivers/rokid/ src/xrt/auxiliary/math/m_imu_3dof.c 2>/dev/null)" ]; then
	echo "==> reverting previous patch state"
	git checkout -- src/xrt/drivers/rokid/ src/xrt/auxiliary/math/m_imu_3dof.c
fi

echo "==> applying patch: $PATCH"
git apply --check "$PATCH"
git apply "$PATCH"

if [ ! -d "$BUILD" ]; then
	echo "==> configuring fresh build dir"
	if [ -z "${CMAKE:-}" ] && ! command -v cmake >/dev/null 2>&1; then
		echo "==> no cmake on PATH, retrying inside nix-shell"
		exec nix-shell "$HERE/monado-shell.nix" --run "'$0'"
	fi
	cmake -S "$MONADO" -B "$BUILD" -G Ninja -DXRT_FEATURE_SERVICE=OFF
fi

echo "==> building (monado-cli + openxr runtime)"
if ! command -v ninja >/dev/null 2>&1; then
	echo "==> no ninja on PATH, retrying inside nix-shell"
	exec nix-shell "$HERE/monado-shell.nix" --run "'$HERE/update-driver.sh'"
fi
ninja -C "$BUILD" monado-cli libopenxr_monado.so

echo "==> done. run: nix-shell $HERE/monado-shell.nix"
echo "    then:      $HERE/run-helloxr.sh"
