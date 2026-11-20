#!/bin/sh
#
# Console (VT) drill for the fbdev driver in QEMU.
#
# Builds a minimal initramfs around the statically linked test_fbdev, boots a
# Debian kernel with a VGA console (vesafb -> fbcon, the setup a machine
# without KMS uses), and runs the test's console phase inside the guest, where
# KD_/VT_ ioctls are real. The guest prints VT-CHECK lines and the harness
# decides PASS/FAIL from them plus a quirk check of the kernel log.
#
#   tests/run_fbdev_qemu.sh [workdir]
#
# Environment:
#   SDLOP_QEMU_KERNEL   vmlinuz to boot (default: newest /boot/vmlinuz-*)
#   SDLOP_QEMU_VIDEO    QEMU -vga value (default std; cirrus and virtio reject
#                       the legacy vga= mode the guest needs for a framebuffer)
#   SDLOP_QEMU_VGA      legacy VESA mode (default 791: 1024x768x16)
#   SDLOP_QEMU_MEM      guest memory in MB (default 1024)
#   SDLOP_QEMU_TIMEOUT  boot+run timeout in seconds (default 300)
#   SDLOP_QEMU_KEEP=1   leave the initramfs and the log in the workdir
#   SDLOP_FBDEV_OWNED_CHECK=1
#                       also run tests/qemu/owned_vt.c: another program owns the
#                       console (KD_GRAPHICS + VT_PROCESS) and the driver must
#                       leave that state alone after SDL_Quit
#   SDLOP_QEMU_CTTY=1   start on controlling VT 3 while VT 2 remains visible
#   SDLOP_QEMU_CHORD=1  inject Ctrl+Alt+F3 into the virtual keyboard and check
#                       the VT moves (needs SDLOP_FBDEV_RAW=1)
#   SDLOP_FBDEV_RAW=1   leave raw input enabled instead of SDLOP_DISABLE_RAW_INPUT=1
#                       (the keyboard policy differs: K_OFF only with a raw
#                       keyboard, and Ctrl+Alt+Fn is then forwarded by hand)
#
# Exits 0 when the drill passed, 1 when a check failed, 2 when the rig itself
# could not be built (missing tools/kernel).
#
# This software is provided 'as-is', without any express or implied
# warranty (zlib license, see LICENSE).

set -eu

SRC=$(cd "$(dirname "$0")/.." && pwd)
WORK=${1:-${TMPDIR:-/tmp}/sdlop-fbdev-qemu}
KERNEL=${SDLOP_QEMU_KERNEL:-}
VIDEO=${SDLOP_QEMU_VIDEO:-std}
MEM=${SDLOP_QEMU_MEM:-1024}
BOOT_TIMEOUT=${SDLOP_QEMU_TIMEOUT:-300}

say() { printf 'harness: %s\n' "$*"; }
die() { printf 'harness: error: %s\n' "$*" >&2; exit 2; }

command -v qemu-system-x86_64 >/dev/null 2>&1 || die "qemu-system-x86_64 not found"
command -v cpio >/dev/null 2>&1 || die "cpio not found"
command -v busybox >/dev/null 2>&1 || die "busybox not found (busybox-static recommended)"

if [ -z "$KERNEL" ]; then
    # newest installed kernel; the .deb can also be unpacked anywhere and
    # pointed at with SDLOP_QEMU_KERNEL
    for candidate in $(ls -1 /boot/vmlinuz-* 2>/dev/null | sort -V -r); do
        KERNEL=$candidate
        break
    done
fi
[ -n "$KERNEL" ] && [ -r "$KERNEL" ] || die "no kernel to boot (set SDLOP_QEMU_KERNEL)"

say "kernel $KERNEL"
say "workdir $WORK"

rm -rf "$WORK"
mkdir -p "$WORK/root/bin" "$WORK/root/dev" "$WORK/root/proc" "$WORK/root/sys" "$WORK/root/tmp" \
         "$WORK/root/modules"

# --- build the test as a static binary (the guest has no libc) --------------
BUILD=${SDLOP_QEMU_BUILD:-$WORK/build}
if [ ! -f "$BUILD/libSDLop.a" ] && [ ! -f "$BUILD/libsdlop.a" ]; then
    say "configuring a build in $BUILD"
    cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Debug >"$WORK/cmake.log" 2>&1 ||
        die "cmake configure failed (see $WORK/cmake.log)"
fi
cmake --build "$BUILD" --target sdlop >"$WORK/build.log" 2>&1 || die "library build failed (see $WORK/build.log)"
LIB=$(ls -1 "$BUILD"/libSDLop.a "$BUILD"/libsdlop.a 2>/dev/null | head -1)
[ -n "$LIB" ] || die "library not found in $BUILD"
say "static test binary"
cc -static -O1 -o "$WORK/root/bin/test_fbdev" "$SRC/tests/test_fbdev.c" \
    -I"$SRC/src" -I"$SRC/include" -D_GNU_SOURCE \
    -DSDLop_VIDEO_FBDEV=1 -DSDLop_VIDEO_KMSDRM=1 -DSDLop_VIDEO_WAYLAND=1 -DSDLop_VIDEO_X11=1 \
    "$LIB" -lpthread -lm -ldl 2>"$WORK/cc.log" || die "static link failed (see $WORK/cc.log)"
if [ "${SDLOP_FBDEV_OWNED_CHECK:-}" = "1" ]; then
cc -static -O1 -o "$WORK/root/bin/owned" "$SRC/tests/qemu/owned_vt.c" \
    -I"$SRC/src" -I"$SRC/include" -D_GNU_SOURCE \
    -DSDLop_VIDEO_FBDEV=1 -DSDLop_VIDEO_KMSDRM=1 -DSDLop_VIDEO_WAYLAND=1 -DSDLop_VIDEO_X11=1 \
    "$LIB" -lpthread -lm -ldl 2>"$WORK/cc-owned.log" || die "owned-VT drill link failed (see $WORK/cc-owned.log)"
fi

cp "$(command -v busybox)" "$WORK/root/bin/busybox"
for applet in sh mount umount ls cat echo sleep mknod chvt poweroff dmesg insmod setsid; do
    ln -sf busybox "$WORK/root/bin/$applet"
done

# --- modules for a virtual keyboard (only needed for the chord drill) -------
# The fbdev rig boots a plain initramfs; evdev and virtio_input are modules on
# Debian kernels, so they are staged next to the kernel and insmod-ed by path.
MODTREE=${SDLOP_QEMU_MODULES:-}
if [ -z "$MODTREE" ]; then
    kver=$(basename "$KERNEL" | sed 's/^vmlinuz-//')
    for candidate in "/usr/lib/modules/$kver" "/tmp/kern/usr/lib/modules/$kver"; do
        [ -d "$candidate/kernel" ] && MODTREE=$candidate && break
    done
fi
MODULES=""
if [ -n "$MODTREE" ]; then
    for name in input-core evdev virtio_input; do
        file=$(find "$MODTREE/kernel" -name "$name.ko*" | head -1)
        [ -n "$file" ] || continue
        case $file in
            *.xz) xz -dc "$file" >"$WORK/root/modules/$name.ko" ;;
            *)    cp "$file" "$WORK/root/modules/$name.ko" ;;
        esac
        MODULES="$MODULES $name"
    done
fi
say "keyboard modules:${MODULES:- none (chord drill will skip)}"

# --- init: mount, take the VT path a real machine takes, run the drill ------
cat >"$WORK/root/init" <<'INIT'
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mkdir -p /dev/pts && mount -t devpts devpts /dev/pts
exec >/dev/ttyS0 2>&1
echo "1" > /proc/sys/kernel/printk

echo "VTHARNESS: up"
for m in __MODULES__; do
    insmod "/modules/$m.ko" 2>/tmp/err && echo "VTHARNESS: mod $m ok" || echo "VTHARNESS: mod $m -> $(cat /tmp/err)"
done
echo "VTHARNESS: input devices: $(ls /dev/input 2>/dev/null | tr '\n' ' ')"
echo "VTHARNESS: fb0 name=$(cat /sys/class/graphics/fb0/name 2>/dev/null) size=$(cat /sys/class/graphics/fb0/virtual_size 2>/dev/null) bpp=$(cat /sys/class/graphics/fb0/bits_per_pixel 2>/dev/null)"

# a shell where a user would be sitting, so "did SDL take the VT away?" has
# an answer, and so fbcon is really the console owner
chvt 2 && echo "VTHARNESS: user on VT 2"
sleep 1

# Knobs are substituted by the host after this quoted heredoc.
export SDL_VIDEODRIVER=fbdev
export SDLOP_DISABLE_RAW_INPUT="__DISABLE_RAW__"
export SDLOP_FBDEV_VT_CHECK=1
export SDLOP_FBDEV_VT_CHECK_TTY=/dev/tty0
export SDLOP_QEMU_CHORD="__CHORD__"
export SDLOP_QEMU_CTTY="__CTTY__"

export SDL_LOGGING=${SDLOP_QEMU_LOGGING:-info}

if [ "$SDLOP_QEMU_CTTY" = "1" ]; then
    setsid sh -c 'exec /bin/test_fbdev </dev/tty3'
else
    /bin/test_fbdev
fi
echo "VTHARNESS: exit=$?"

# A console that already belongs to another program: KD_GRAPHICS plus
# VT_PROCESS on VT 2. The driver must refuse it, leave that program's state
# alone, and still be able to draw through /dev/fb0.
if [ -x /bin/owned ]; then
    echo "VTHARNESS: owned-VT drill"
    /bin/owned
    echo "VTHARNESS: owned exit=$?"
fi
poweroff -f
INIT
sed -i "s/__MODULES__/${MODULES:-}/" "$WORK/root/init"
DISABLE_RAW=1
[ "${SDLOP_FBDEV_RAW:-0}" = "1" ] && DISABLE_RAW=0
sed -i "s/__DISABLE_RAW__/$DISABLE_RAW/; s/__CHORD__/${SDLOP_QEMU_CHORD:-0}/; s/__CTTY__/${SDLOP_QEMU_CTTY:-0}/" "$WORK/root/init"
chmod +x "$WORK/root/init"

# the guest kernel is booted without modules: a console framebuffer needs
# CONFIG_FB_VESA or CONFIG_FB_EFI, both built in on Debian's kernels
( cd "$WORK/root" && find . | cpio -o -H newc --quiet 2>/dev/null | gzip -9 ) >"$WORK/initramfs.gz" ||
    die "initramfs failed"

# --- Ctrl+Alt+Fn: type the chord into the virtual keyboard -------------------
# With raw input on (SDLOP_FBDEV_RAW=1) the console keyboard sits in K_OFF and
# only sdlop can see Ctrl+Alt+Fn; the guest prints VT-CHORD-READY and the chord
# is typed through the QEMU monitor.
CHORD=${SDLOP_QEMU_CHORD:-0}
inject_chord() {
    i=0
    while [ "$i" -lt 600 ]; do
        [ -s "$WORK/serial.log" ] && grep -aq '^VT-CHORD-READY' "$WORK/serial.log" && break
        i=$((i + 1))
        sleep 0.1
    done
    if ! grep -aq '^VT-CHORD-READY' "$WORK/serial.log" 2>/dev/null; then
        say "the guest never reached the chord phase"
        return 0
    fi
    { printf 'sendkey ctrl-alt-f3\n'; sleep 0.5; } |
        socat -t 3 - "UNIX-CONNECT:$WORK/mon.sock" >/dev/null 2>&1 ||
        say "could not talk to the QEMU monitor"
    say "Ctrl+Alt+F3 injected into the virtio keyboard"
}

# --- boot ------------------------------------------------------------------
say "booting ($VIDEO, ${MEM} MB, ${BOOT_TIMEOUT}s cap)"
rm -f "$WORK/mon.sock" "$WORK/serial.log"
CHORD_PID=
if [ "$CHORD" = "1" ]; then
    command -v socat >/dev/null 2>&1 || die "socat not found (needed for the chord drill)"
    inject_chord &
    CHORD_PID=$!
fi
set +e
timeout "$BOOT_TIMEOUT" qemu-system-x86_64 \
    -M q35 -m "$MEM" -smp 2 \
    -kernel "$KERNEL" -initrd "$WORK/initramfs.gz" \
    -append "console=ttyS0,115200 console=tty0 panic=1 fbcon=nodefer vga=${SDLOP_QEMU_VGA:-791} loglevel=4" \
    -display none -vga "$VIDEO" -no-reboot \
    -device "virtio-keyboard-pci,id=kbd0" \
    -monitor "unix:$WORK/mon.sock,server=on,wait=off" \
    -serial "file:$WORK/serial.log"
rc=$?
if [ -n "$CHORD_PID" ]; then
    kill "$CHORD_PID" 2>/dev/null || true
    wait "$CHORD_PID" 2>/dev/null || true
fi
set -e
case $rc in
    0) ;;
    124) say "guest did not finish within ${BOOT_TIMEOUT}s" ;;
    *)   say "qemu exited with $rc" ;;
esac

if [ ! -s "$WORK/serial.log" ]; then
    say "no serial output at all; the rig could not run"
    exit 2
fi

# --- verdict ---------------------------------------------------------------
grep -a '^  ok:' "$WORK/serial.log" | sed 's/^/  /' || true
grep -a '^FAIL' "$WORK/serial.log" | sed 's/^/  /' || true
grep -a 'VTHARNESS:\|fbdev:' "$WORK/serial.log" | sed 's/^/  /' || true

ok=$(grep -ac '^  ok:' "$WORK/serial.log" || true)
bad=$(grep -ac '^FAIL' "$WORK/serial.log" || true)
# the owned-VT drill reports its own VT-CHECK lines
chec_ok=$(grep -ac '^VT-CHECK .* PASS' "$WORK/serial.log" || true)
chec_bad=$(grep -ac '^VT-CHECK .* FAIL' "$WORK/serial.log" || true)
if [ "$chec_ok" != "0" ] || [ "$chec_bad" != "0" ]; then
    printf 'owned-VT drill: %s pass / %s fail\n' "$chec_ok" "$chec_bad"
    ok=$((ok + chec_ok))
    bad=$((bad + chec_bad))
fi
printf 'guest: %s pass / %s fail\n' "$ok" "$bad"

# a console driver that hits a kernel WARN/BUG while owning the VT is broken
# even if its own checks passed
quirk=$(grep -acE 'WARNING:|BUG:|Call Trace:|Oops' "$WORK/serial.log" || true)
if [ "$quirk" != "0" ]; then
    printf 'guest: %s kernel WARNING/BUG lines (see %s/serial.log)\n' "$quirk" "$WORK"
    bad=$((bad + quirk))
fi

if grep -aq 'VTHARNESS: exit=0' "$WORK/serial.log" && [ "$bad" = "0" ] && [ "$ok" != "0" ]; then
    printf 'RESULT: PASS\n'
    [ "${SDLOP_QEMU_KEEP:-0}" = "1" ] || rm -rf "$WORK/root" "$WORK/build"
    exit 0
fi

printf 'RESULT: FAIL (log: %s/serial.log)\n' "$WORK"
exit 1
