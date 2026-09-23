#!/bin/sh
# Run test_web in headless chromium (real DOM + WebGL via SwiftShader).
#
# Usage: tests/run_browser_test.sh [build-web-dir]
#
# Serves the build dir over HTTP (file:// is blocked by wasm CORS),
# runs chromium --headless --dump-dom and checks for the PASS marker.
set -e

DIR="${1:-build-web}"
PORT="${PORT:-8123}"
CHROME="${CHROME:-chromium}"

cd "$(dirname "$0")/.."

cat > "$DIR/test_web_shell.html" << 'EOF'
<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>SDLop test_web</title>
<style>body{margin:0}</style>
</head>
<body>
<canvas id="canvas" width="640" height="480" oncontextmenu="event.preventDefault()"></canvas>
<script src="test_web.js"></script>
</body></html>
EOF

python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$DIR" &
SRV=$!
trap 'kill $SRV 2>/dev/null' EXIT
sleep 1

OUT=$(timeout 90 "$CHROME" --headless=new --no-sandbox --disable-gpu-sandbox \
    --enable-unsafe-swiftshader --virtual-time-budget=10000 --dump-dom \
    "http://127.0.0.1:$PORT/test_web_shell.html" 2>/dev/null \
    | sed -n '/sdlop-test-out/,/<\/pre>/p')

echo "$OUT"
case "$OUT" in
    *"test_web: PASS"*) exit 0 ;;
    *) exit 1 ;;
esac
