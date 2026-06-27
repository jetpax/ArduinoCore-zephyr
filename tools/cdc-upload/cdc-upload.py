#!/usr/bin/env python3
# Copyright (c) 2026 Jonathan Elliot Peace <jep@alphabetiq.com>
# SPDX-License-Identifier: Apache-2.0
#
# PiZZa Phase-6 host uploader. Pushes a sketch .llext/.elf to the PiZZa loader
# over USB CDC, in-process and with no reboot:
#
#   1) 1200-bps touch: the IDE's standard "enter upload mode" signal. Open the
#      port at 1200 baud, hold ~0.6 s so the loader's baud poll sees it, close.
#      The loader aborts the running sketch and switches to upload-listen.
#   2) Reopen at normal baud and send: "PZUP" + <le32 length> + <bytes>.
#      The loader writes it to /SD:/sketch.llext and replies "OK\n", then
#      starts the new sketch in its own thread.
#
# This is the proven spike protocol (PiZZa/os/Arduino/spikes/cdc-loader);
# the tool does the touch itself, so boards.txt sets use_1200bps_touch=false.
#
# Requires pyserial. Usage:
#   cdc-upload.py <serial-port> <sketch.elf>

import struct
import sys
import time

try:
    import serial
except ImportError:
    sys.exit(
        "cdc-upload: pyserial is required (pip install pyserial), "
        "or run via a python with pyserial available."
    )

TOUCH_BAUD = 1200
DATA_BAUD = 115200
TOUCH_HOLD_S = 0.6
TOUCH_SETTLE_S = 0.4
REPLY_TIMEOUT_S = 8


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: cdc-upload.py <serial-port> <sketch.elf>")

    port, path = sys.argv[1], sys.argv[2]
    with open(path, "rb") as fh:
        data = fh.read()

    # 1) 1200-bps touch.
    print(f"cdc-upload: 1200-bps touch on {port}...")
    try:
        t = serial.Serial(port, TOUCH_BAUD)
        time.sleep(TOUCH_HOLD_S)
        t.close()
    except serial.SerialException as e:
        sys.exit(f"cdc-upload: touch failed: {e}")
    time.sleep(TOUCH_SETTLE_S)  # let the loader enter upload-listen

    # 2) Reopen and send the framed sketch.
    try:
        s = serial.Serial(port, DATA_BAUD, timeout=REPLY_TIMEOUT_S)
    except serial.SerialException as e:
        sys.exit(f"cdc-upload: reopen failed: {e}")
    time.sleep(0.2)
    s.reset_input_buffer()
    s.write(b"PZUP" + struct.pack("<I", len(data)) + data)
    s.flush()
    print(f"cdc-upload: sent {len(data)} bytes; waiting for device...")

    line = s.readline().decode(errors="replace").strip()
    print("cdc-upload: device:", line if line else "(no response)")
    s.close()
    sys.exit(0 if line == "OK" else 1)


if __name__ == "__main__":
    main()
