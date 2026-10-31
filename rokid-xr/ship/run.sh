#!/usr/bin/env bash
# run.sh - launch on the glasses from the portable bundle.
#   ./run.sh                # grid-floor hello_xr
#   ./run.sh <app> [args]   # any OpenXR app (xrgears, blender, ...)
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
# nix-portable v010 keeps everything under $NP_LOCATION/.nix-portable
# (default: $HOME). Override with NP_LOCATION if $HOME is tight on space.
# NOTE: `nix-portable <cmd>` only runs nix-distro binaries; arbitrary
# programs go through the `debug` subcommand (used in run.sh).

# Bundled Mesa ICDs, rewritten to absolute store paths (soname bare
# refs don't resolve under the nix loader).
if [ -z "${VK_ICD_FILENAMES:-}" ]; then
  export VK_ICD_FILENAMES=$(ls "$HERE"/store-icds/*.json | tr '\n' ':')
  export VK_ICD_FILENAMES="${VK_ICD_FILENAMES%:}"
fi

case "${1:-}" in
  ""|hello|hello_xr|helloxr)
    exec "$HERE/nix-portable" bash "$HERE/rokid-xr/run-helloxr.sh" "${@:2}"
    ;;
  list)
    exec "$HERE/nix-portable" bash "$HERE/rokid-xr/run-helloxr.sh" list
    ;;
  *)
    APP="$1"; shift
    exec "$HERE/nix-portable" bash "$HERE/rokid-xr/run-openxr.sh" "$APP" "$@"
    ;;
esac
