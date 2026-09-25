#!/bin/bash
# run_bp.sh <probe-binary> <outfile> [extra probe args...]
PROBE=$1; OUT=$2; shift 2
SOCK=$(ls /tmp/xdg-sway/sway-ipc.*.sock | head -1)
rm -f "$OUT"
SDLOP_DISABLE_RAW_INPUT=1 XDG_RUNTIME_DIR=/tmp/xdg-sway WAYLAND_DISPLAY=wayland-1 \
  "$PROBE" "$@" > "$OUT" 2>/dev/null &
PID=$!
for i in $(seq 1 100); do grep -q "READY-INPUT" "$OUT" 2>/dev/null && break; sleep 0.1; done
grep -q "READY-INPUT" "$OUT" || { echo "no READY-INPUT"; kill $PID; exit 1; }
# locate the window through the compositor tree and aim the script inside it
RECT=$(SWAYSOCK=$SOCK swaymsg -t get_tree | python3 -c "
import json,sys
t=json.load(sys.stdin)
res=[]
def walk(n):
    if n.get('pid') and n.get('rect'): res.append((n['rect'], n.get('focused')))
    for c in n.get('nodes',[])+n.get('floating_nodes',[]): walk(c)
walk(t)
for r,f in res: print(r['x'], r['y'], r['width'], r['height'], f)
")
echo "TREE: $RECT"
set -- $RECT
RX=$1; RY=$2; RW=$3; RH=$4; RF=$5
# aim inside the smallest surface both implementations can have: the
# container may be larger than the attached buffer (stock defers buffers),
# so use small offsets from the container origin instead of fractions
CX=$((RX + 100)); CY=$((RY + 80))
QX=$((RX + 60));  QY=$((RY + 50))
cat > /tmp/bp_script.txt << SCR
sleep 400
cursor $CX $CY
sleep 100
click left
sleep 150
cursor $CX $CY
sleep 200
cursor $QX $QY
sleep 200
move 10 -5
sleep 200
button left down
sleep 120
button left up
sleep 200
wheel 3
sleep 200
wheel -3
sleep 200
cursor 5000 3000
sleep 250
cursor $QX $QY
sleep 200
tap a
sleep 150
tap w
sleep 150
key lshift down
sleep 80
tap e
sleep 80
key lshift up
sleep 200
tap enter
sleep 150
tap esc
sleep 250
SCR
XDG_RUNTIME_DIR=/tmp/xdg-sway WAYLAND_DISPLAY=wayland-1 /tmp/injector /tmp/bp_script.txt
wait $PID
echo "exit=$?"
