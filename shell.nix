{ pkgs ? import <nixpkgs> { } }:

pkgs.mkShell {
  buildInputs = with pkgs; [
    uv

    # USB device listing + descriptors
    usbutils        # lsusb, usb-devices

    # packet capture on usbmon
    tshark          # CLI wireshark; capture + dissect usbmon traffic

    # scripting control/replay
    (python3.withPackages (ps: with ps; [
      pyusb         # libusb control/bulk transfers
      hid           # HID API (if control interface is HID)
      pyserial      # CDC-ACM serial (if control interface is CDC)
    ]))

    # usbmon needs debugfs mounted + loadable on host kernel
    kmod            # modprobe usbmon
  ];

  shellHook = ''
    echo "== USB env check =="
    command -v lsusb || true
    ls /sys/kernel/debug/usbmon 2>/dev/null || echo "usbmon not loaded/mounted yet (see docker notes)"
    ls /dev/bus/usb 2>/dev/null || echo "no /dev/bus/usb -> device nodes not passed into container"
  '';
}
