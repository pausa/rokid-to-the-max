#!/usr/bin/env bash
# make-bundle.sh - build a self-contained portable bundle (run in a
# nix-capable environment, e.g. the container). Output: a directory
# (default ../rokid-bundle) + a compressed tarball. The bundle carries:
#   - rokid-xr project (scripts, docs, patches, BUILT runtime artifacts)
#   - the full /nix/store closure of the runtime (own glibc/mesa/vulkan)
#   - nix-portable (static, mounts /nix/store via user namespaces, proot
#     fallback) so it runs on ANY Linux distro without root or nix.
# Target usage: untar, ./bootstrap.sh, ./run.sh (see SHIP.md).
set -euo pipefail
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)   # rokid-xr/
OUT=${1:-"$HERE/../rokid-bundle"}
NP_VERSION=${NP_VERSION:-v010}

rm -rf "$OUT"; mkdir -p "$OUT/store" "$OUT/store-icds"

echo "==> 1/5 copying project files (excluding third_party sources)"
mkdir -p "$OUT/rokid-xr/third_party"
( cd "$HERE" && find . -maxdepth 1 -exec cp -a {} "$OUT/rokid-xr/" \; )
rm -rf "$OUT/rokid-xr/third_party/monado" "$OUT/rokid-xr/third_party/openxr-sdk-source"
# build outputs are wanted:
mkdir -p "$OUT/rokid-xr/third_party/monado" "$OUT/rokid-xr/third_party/openxr-sdk-source"
cp -a "$HERE/third_party/monado/build-host" "$OUT/rokid-xr/third_party/monado/"
cp -a "$HERE/third_party/openxr-sdk-source/build-hr" "$OUT/rokid-xr/third_party/openxr-sdk-source/"

echo "==> 2/5 computing nix store closure"
BINS=(
  "$HERE/third_party/monado/build-host/src/xrt/targets/openxr/libopenxr_monado.so"
  "$HERE/third_party/monado/build-host/src/xrt/targets/cli/monado-cli"
  "$HERE/third_party/openxr-sdk-source/build-hr/src/tests/hello_xr/hello_xr"
)
: > "$OUT/store/manifest.txt"
{
  for b in "${BINS[@]}"; do
    ldd "$b" 2>/dev/null | grep -oE "/nix/store/[^ :]+" || true
  done
  # mesa (Vulkan ICDs) - loaded at runtime via VK_ICD_FILENAMES, invisible
  # to ldd. Resolve the same way monado-shell.nix does.
  nix-shell "$HERE/monado-shell.nix" --run 'echo "$VK_ICD_FILENAMES"' 2>/dev/null \
    | tr ':' '\n' | grep -oE "^/nix/store/[^/]+" | sort -u
} | sort -u > /tmp/bundle-refs.txt
# Seed with qR of every resolved path, then run a fixpoint over direct
# references: some deps (e.g. glibc's libidn2) never appear in any
# binary's ldd and qR can silently miss them - add everything until
# stable, else the import on the target fails with "path not valid".
{
  for r in $(cat /tmp/bundle-refs.txt); do
    nix-store -qR "$r" 2>/dev/null || true
  done
} | awk '!seen[$0]++' > /tmp/bundle-closure.txt
changed=1
while [ "$changed" = 1 ]; do
  changed=0
  while read -r p; do
    for d in $(nix-store -q --references "$p" 2>/dev/null); do
      if ! grep -qxF "$d" /tmp/bundle-closure.txt; then
        echo "$d" >> /tmp/bundle-closure.txt
        changed=1
      fi
    done
  done < /tmp/bundle-closure.txt
done

echo "==> 3/5 exporting $(wc -l < /tmp/bundle-closure.txt) store paths"
rm -f "$OUT/store/manifest.txt"
i=0
while read -r p; do
  i=$((i+1))
  nix-store --export "$p" > "$OUT/store/$(printf '%04d' $i).nar-export"
  echo "$p" >> "$OUT/store/manifest.txt"
done < /tmp/bundle-closure.txt

echo "==> 4/5 rewriting Vulkan ICD JSONs to absolute store paths"
ICDS=$(nix-shell "$HERE/monado-shell.nix" --run 'echo "$VK_ICD_FILENAMES"' 2>/dev/null)
OLDIFS=$IFS
for json in ${ICDS//:/ }; do
  case "$json" in
    *radeon*|*intel_icd*|*lavapipe*|*lvp*) ;;
    *) continue ;;
  esac
  name=$(basename "$json")
  # json lives at <mesa-root>/share/vulkan/icd.d/<name>
  mesa_root=$(dirname "$(dirname "$(dirname "$json")")")
  sed "s|\"library_path\"[[:space:]]*:[[:space:]]*\"\([^\"/][^\"]*\)\"|\"library_path\" : \"$mesa_root/lib/\1\"|" "$json" > "$OUT/store-icds/$name"
done
IFS=$OLDIFS
ls "$OUT/store-icds/" || true

echo "==> 5/5 fetching nix-portable ($NP_VERSION)"
curl -fL -o "$OUT/nix-portable" "https://github.com/DavHau/nix-portable/releases/download/$NP_VERSION/nix-portable"
chmod +x "$OUT/nix-portable"
cp "$HERE/ship/60-rokid-max.rules" "$OUT/"
cp "$HERE/ship/bootstrap.sh" "$OUT/"
cp "$HERE/ship/run.sh" "$OUT/"
chmod +x "$OUT/bootstrap.sh" "$OUT/run.sh"

echo "==> bundling tarball"
TARBALL="$OUT.tar.zst"
if command -v zstd >/dev/null; then
  tar -C "$(dirname "$OUT")" --use-compress-program='zstd -T0 -19' -cf "$TARBALL" "$(basename "$OUT")"
else
  nix-shell -p zstd --run "tar -C '$(dirname "$OUT")' --use-compress-program='zstd -T0 -19' -cf '$TARBALL' '$(basename "$OUT")'"
fi
du -sh "$OUT" "$TARBALL"
echo "bundle ready: $TARBALL"
