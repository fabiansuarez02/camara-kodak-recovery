#!/bin/sh
# Cross-compile the streamer for the MT7620A (MIPS mipsel, kernel 2.6.36).
#
# Uses `zig cc` as a zero-hassle cross toolchain:
#   pip install ziglang        # provides `python -m ziglang cc`
#
# Static + musl + size-optimized. Resulting binary is ~60-68 KB and runs
# on the stock 2.6.36 kernel.

set -e
python3 -m ziglang cc \
    -target mipsel-linux-musleabi \
    -static -Os \
    mjpgstream.c -o mjpgstream

echo "built: $(ls -l mjpgstream | awk '{print $5}') bytes"
file mjpgstream || true
