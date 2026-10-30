#!/usr/bin/env python3
"""Read-only probe of Rokid Max (04d2:162f) vendor control transfers + CDC-ACM.

Based on xelsed/rokidmaxwmdapi findings for Rokid Max2 (same VID):
GET = bRequestType 0xC1, SET = 0x41. AVOID command 0x03 (disconnects device on Max2).
Only GETs here. If the glasses vanish from lsusb mid-run, replug.
"""
import os, sys, time
import usb.core, usb.util

VID, PID = 0x04D2, 0x162F

def find():
    return usb.core.find(idVendor=VID, idProduct=PID)

dev = find()
if dev is None:
    sys.exit("Rokid Max not found (replug + check lsusb)")
print(f"found: bus {dev.bus} dev {dev.address}, {len(dev.configurations())} config(s)")

# libusb driver claim: detach kernel drivers where possible
if dev.is_kernel_driver_active(0):
    try:
        dev.detach_kernel_driver(0)
    except usb.core.USBError as e:
        print("could not detach kernel driver on if0:", e)

results = {}
SAFE_CMDS = [0x00, 0x01, 0x02, 0x04, 0x05, 0x10, 0x20, 0x40, 0x80, 0xF0]

def probe_iface(iface):
    print(f"\n--- interface {iface} ---")
    for cmd in SAFE_CMDS:
        try:
            data = dev.ctrl_transfer(
                0xC1,      # vendor, device-to-host, interface recipient
                cmd,
                wValue=0,
                wIndex=iface,
                data_or_wLength=64,
                timeout=150,
            )
            b = bytes(data)
            nz = b.rstrip(b"\x00")
            tag = "ALL-ZERO" if not nz else nz.hex()
            results[(iface, cmd)] = b
            print(f"  cmd 0x{cmd:02x}: {tag[:96]}")
        except usb.core.USBError as e:
            print(f"  cmd 0x{cmd:02x}: {e}")

for i in (0, 1, 2):   # 3 is the CDC control interface; Max2 crashed on if3, skip
    probe_iface(i)

# --- CDC-ACM serial (interface 3 controls it) ---
print("\n--- CDC-ACM ---")
try:
    import serial, serial.tools.list_ports
    ports = [p.device for p in serial.tools.list_ports.comports()
             if p.vid == VID and p.pid == PID]
    print("ports:", ports or "none visible (need /dev/ttyACM* passed to container)")
except ImportError:
    print("pyserial not available")

print("\nsurvived, device still present:", find() is not None)
