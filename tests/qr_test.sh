#!/bin/bash
# QR encoder (src/qr.inc.c) against python-qrcode, module by module, plus an OpenCV decode.
#   bash tests/qr_test.sh
# The Python packages go into a private virtualenv ($QR_VENV, default ~/.cache/hibyos_qr_venv).
set -e
cd "$(dirname "$0")/.."
gcc -O1 -g -Wall -Wextra -fsanitize=address,undefined -o /tmp/qr_test tests/qr_test.c
V=${QR_VENV:-$HOME/.cache/hibyos_qr_venv}
if [ ! -x "$V/bin/python" ]; then python3 -m venv "$V"; fi
"$V/bin/python" -c 'import qrcode, cv2, numpy' 2>/dev/null || "$V/bin/pip" -q install qrcode opencv-python-headless numpy
"$V/bin/python" tests/qr_check.py /tmp/qr_test
