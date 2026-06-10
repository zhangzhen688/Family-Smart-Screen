#!/bin/bash
# One-click startup for Smart Screen + XiaoZhi AI.
# Press Ctrl+C to stop all services.
set -e

ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$ROOT/build"

# Ensure runtime assets are available
mkdir -p "$BUILD_DIR/assets" "$BUILD_DIR/photos"
cp -n "$ROOT/gui/assets/"*.png "$BUILD_DIR/assets/" 2>/dev/null || true
cp -n "$ROOT/gui/assets/"*.ttf "$BUILD_DIR/assets/" 2>/dev/null || true

PIDS=""

cleanup() {
    echo ""
    echo "==> Shutting down all services..."
    for pid in $PIDS; do
        kill "$pid" 2>/dev/null
    done
    wait 2>/dev/null
    echo "==> Done."
    exit 0
}

trap cleanup SIGINT SIGTERM

echo "=============================================="
echo " Smart Screen + XiaoZhi AI — Starting..."
echo "=============================================="
echo ""

# 0. Web remote control server
echo "[0/7] Web Remote Control (port 8080)"
"$BUILD_DIR/backend/web_server" &
PIDS="$PIDS $!"
sleep 1

# 1. Device server
echo "[1/7] Device Server (port 1234)"

"$BUILD_DIR/backend/device_server" &
PIDS="$PIDS $!"
sleep 1

# 2. Camera server
echo "[2/7] Camera Server (port 1235)"
"$BUILD_DIR/backend/camera_server" &
PIDS="$PIDS $!"
sleep 1

# 3. Voice server (JSON-RPC bridge)
echo "[3/7] Voice RPC Server (port 1236)"
"$BUILD_DIR/backend/voice_server" &
PIDS="$PIDS $!"
sleep 1

# 4. XiaoZhi control center (WebSocket → cloud)
if [ -x "$BUILD_DIR/backend/xiaozhi_control_center" ]; then
    echo "[4/7] XiaoZhi Control Center (UDP 5678/5679)"
    "$BUILD_DIR/backend/xiaozhi_control_center" &
    PIDS="$PIDS $!"
    sleep 2
else
    echo "[4/7] XiaoZhi Control Center — SKIP (not built)"
fi

# 5. XiaoZhi sound app (ALSA audio)
if [ -x "$BUILD_DIR/backend/xiaozhi_sound_app" ]; then
    echo "[5/7] XiaoZhi Sound App (ALSA audio)"
    "$BUILD_DIR/backend/xiaozhi_sound_app" &
    PIDS="$PIDS $!"
    sleep 1
else
    echo "[5/7] XiaoZhi Sound App — SKIP (not built)"
fi

# 6. GUI
echo "[6/7] Smart Screen GUI"
cd "$BUILD_DIR"
"$BUILD_DIR/gui/smart_gui" &
PIDS="$PIDS $!"

echo ""
echo "=============================================="
echo " All services started!"
echo ""
echo " GUI (SDL):      local window"
echo " Web Remote:     http://<ip>:8080   (open on phone!)"
echo " Device API:     port 1234"
echo " Camera API:     port 1235"
echo " Voice API:      port 1236"
echo " XiaoZhi CC:     UDP 5678/5679"
echo ""
echo " Press Ctrl+C to stop."
echo "=============================================="

# Wait for GUI
wait 2>/dev/null
cleanup
