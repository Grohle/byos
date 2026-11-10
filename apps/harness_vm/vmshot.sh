#!/bin/bash
# Waits for the harness VM (port HIBY_PORT, default 5566) and saves a screenshot.
# Usage: bash vmshot.sh OUT.png
PORT=${HIBY_PORT:-5566}
for i in $(seq 1 40); do
  (exec 3<>/dev/tcp/127.0.0.1/$PORT) 2>/dev/null && break
  sleep 3
done
cd "$(dirname "$0")" && HIBY_PORT=$PORT timeout 90 python3 hibyctl.py shot "$1"
