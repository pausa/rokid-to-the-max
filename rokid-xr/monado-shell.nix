# Minimal build environment for Monado driver development.
# Usage: nix-shell monado-shell.nix --run 'cmake ...'
{ pkgs ? import <nixpkgs> {} }:

let
  # nix Mesa driver stack: its vulkan ICD jsons reference nix-store
  # libraries (absolute paths), so nix-built programs can use the host
  # GPU without nixGL. Same trick nixGL uses.
  vulkanIcdDir = "${pkgs.mesa}/share/vulkan/icd.d";
in
pkgs.mkShell {
  buildInputs = with pkgs; [
    cmake
    ninja
    pkg-config
    python3
    gcc15
    vulkan-headers
    vulkan-loader
    glslang
    eigen
    cjson
    libdrm
    libusb1
    udev
    hidapi
    dbus
    wayland
    wayland-protocols
    libx11
    libxcb
    xcbutilerrors
    xcbutilwm
    libxrandr
    libxxf86vm
    opencv
    openxr-loader
  ];

  shellHook = ''
    # Prefer the nix Mesa ICDs (self-contained, nix-store absolute paths).
    # The run script only uses this if VK_ICD_FILENAMES isn't already set.
    if [ -z "''${VK_ICD_FILENAMES:-}" ]; then
      icds=""
      for f in "${vulkanIcdDir}"/*_icd*.json; do
        [ -f "$f" ] && icds="$icds$f:"
      done
      if [ -n "$icds" ]; then
        export VK_ICD_FILENAMES="''${icds%:}"
        export ROKIDX_VK_SOURCE="nix-mesa"
      fi
    fi
  '';
}
