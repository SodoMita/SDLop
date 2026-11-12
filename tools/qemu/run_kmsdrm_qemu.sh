#!/bin/bash
#
# SDLop KMSDRM QEMU harness - boot a Debian guest with a chosen virtual GPU,
# build SDLop inside it and verify the kmsdrm driver end to end (modesetting,
# dumb-buffer software scanout, GBM/EGL GL scanout, mode switching, fallbacks).
# QEMU itself verifies what the monitor shows via the QMP "screendump" command
# (three horizontal bands painted by tests/test_kmsdrm).
#
# Usage:
#   tools/qemu/run_kmsdrm_qemu.sh install          # one-time: create + unattended-install the guest disk
#   tools/qemu/run_kmsdrm_qemu.sh deploy           # boot guest, build SDLop inside, store build in the guest
#   tools/qemu/run_kmsdrm_qemu.sh scenario <name>  # one scenario end to end
#   tools/qemu/run_kmsdrm_qemu.sh all              # deploy + all scenarios
#
# State (disk image, logs, screendumps) lives in $SDLOP_QEMU_STATE
# (default: ~/.cache/sdlop-qemu). The ISO is downloaded once from
# cdimage.debian.org; guest root password is "root" (local test VM only).
#
# Scenarios. The installed guest only boots reliably with the stock primary
# stdvga (-vga std), so every scenario keeps it and adds named secondary GPUs
# selected with SDLOP_KMSDRM_DEVICE=1. Named devices get a QMP id so the
# harness can screendump exactly the head the driver is expected to use.
#   stdvga        -vga std                                   (std VGA / bochs-drm KMS, default probe)
#   bochs         stdvga + bochs-display,id=bochs0, SDLOP_KMSDRM_DEVICE=1
#   virtio        stdvga + virtio-gpu-pci,id=gpu0, SDLOP_KMSDRM_DEVICE=1
#   virtio-gl     stdvga + virtio-gpu-gl-pci,id=gpu0, SDLOP_KMSDRM_DEVICE=1, xvfb+gtk,gl=on
#   dual-head     stdvga + bochs-display (default probe picks stdvga)
#   card-select   stdvga + bochs-display,id=bochs0, SDLOP_KMSDRM_DEVICE=1
#   no-flip       virtio scenario with SDLOP_KMSDRM_NOFLIP=1
#   bad-device    SDLOP_KMSDRM_DEVICE=99                     (graceful failure, no crash)
#
set -u -o pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
STATE="${SDLOP_QEMU_STATE:-$HOME/.cache/sdlop-qemu}"
DISK="$STATE/disks/sdlop-kms.qcow2"
LOGS="$STATE/logs"
PPM="$STATE/ppm"
SERVE="$STATE/serve"
ISO="$STATE/debian-13.7.0-amd64-netinst.iso"
# cdimage.debian.org is the canonical source but can be extremely slow; the leaseweb mirror carries the same current ISO
ISO_URL="https://mirror.leaseweb.com/debian-cd/current/amd64/iso-cd/debian-13.7.0-amd64-netinst.iso"
VMLINUZ="$STATE/vmlinuz"
INITRD="$STATE/initrd.gz"
QMP_PY="$SCRIPT_DIR/qmp_screendump.py"
SMP="${SDLOP_QEMU_SMP:-2}"
MEM="${SDLOP_QEMU_MEM:-1024}"
mkdir -p "$STATE/disks" "$LOGS" "$PPM" "$SERVE"

SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o LogLevel=ERROR -p 2222)
SCP_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=5 -o LogLevel=ERROR -P 2222)
guest_ssh() { sshpass -proot ssh "${SSH_OPTS[@]}" root@localhost "$@"; }
guest_scp() { sshpass -proot scp "${SCP_OPTS[@]}" "$@"; }

# display modes:
#   none   - headless (default)
#   gtk-gl - virtual X (xvfb) + gtk,gl=on; needed for virtio-gpu-gl-pci
#            (virgl) on hosts without a /dev/dri render node; QEMU's
#            egl-headless requires one, but GLX on Xvfb works via llvmpipe
qemu_common() {
    local mode="${2:-none}"
    local display_args
    case "$mode" in
        gtk-gl) display_args="-display gtk,gl=on" ;;
        *)      display_args="-display none" ;;
    esac
    echo -machine pc,accel=tcg -cpu qemu64 -m "$MEM" -smp "$SMP" \
        -drive file="$DISK",if=virtio,format=qcow2 \
        -netdev user,id=n0,hostfwd=tcp::2222-:22 -device e1000,netdev=n0 \
        $display_args -serial file:"$LOGS/guest-$1.log"
}

qemu_cmd() {
    echo "qemu-system-x86_64"
}

wait_for_ssh() { # $1 = scenario name, $2 = timeout seconds
    local deadline=$(( $(date +%s) + ${2:-240} ))
    while [ "$(date +%s)" -lt "$deadline" ]; do
        if guest_ssh true 2>/dev/null; then
            echo "  ssh up after $((deadline - $(date +%s)))s of headroom"
            return 0
        fi
        sleep 2
    done
    echo "  TIMEOUT waiting for ssh" >&2
    return 1
}

go_install() {
    if [ ! -f "$ISO" ]; then
        echo "downloading $ISO_URL"
        curl -L --retry 3 -o "$ISO" "$ISO_URL" || return 1
    fi
    cp "$SCRIPT_DIR/preseed.cfg" "$SERVE/preseed.cfg"
    if [ ! -f "$VMLINUZ" ]; then
        command -v xorriso >/dev/null || { echo "need xorriso to extract the netboot kernel" >&2; return 1; }
        xorriso -osirrox on -indev "$ISO" \
            -extract /install.amd/vmlinuz "$VMLINUZ" \
            -extract /install.amd/initrd.gz "$INITRD" || return 1
    fi
    [ -f "$DISK" ] || qemu-img create -f qcow2 "$DISK" 10G
    echo "serving preseed on :8080 (ctrl+c when the install powers off)"
    (cd "$SERVE" && exec python3 -m http.server 8080 --bind 0.0.0.0 >"$LOGS/http.log" 2>&1) &
    local http_pid=$!
    # shellcheck disable=SC2086
    qemu-system-x86_64 $(qemu_common install | tr '\n' ' ') \
        -kernel "$VMLINUZ" -initrd "$INITRD" \
        -append "auto=true priority=critical preseed/url=http://10.0.2.2:8080/preseed.cfg console=ttyS0,115200" \
        -cdrom "$ISO" -monitor none -no-reboot
    kill "$http_pid" 2>/dev/null || true
    echo "install finished (guest powered off). State: $STATE"
}

go_deploy() {
    echo "== deploy (boot default GPU, build SDLop in guest) =="
    local qmp="$STATE/qmp-deploy.sock"
    rm -f "$qmp"
    # shellcheck disable=SC2046
    qemu-system-x86_64 $(qemu_common deploy | tr '\n' ' ') \
        -vga std -monitor none -qmp unix:"$qmp",server=on,wait=off -daemonize || return 1
    if ! wait_for_ssh deploy 300; then
        echo "guest did not come up; see $LOGS/guest-deploy.log" >&2
        cleanup_vm "$qmp"
        return 1
    fi
    # sync the source (exclude build dirs and VCS) and rebuild in guest
    local tarball="$STATE/sdlop-src.tar.gz"
    (cd "$REPO_ROOT" && tar --exclude=./build --exclude=./.git --exclude='./SDL3' -czf "$tarball" .)
    guest_ssh "mkdir -p /root/sdlop" || { cleanup_vm "$qmp"; return 1; }
    guest_scp "$tarball" root@localhost:/tmp/sdlop-src.tar.gz || { cleanup_vm "$qmp"; return 1; }
    guest_ssh "rm -rf /root/sdlop && mkdir -p /root/sdlop && tar -xzf /tmp/sdlop-src.tar.gz -C /root/sdlop" || { cleanup_vm "$qmp"; return 1; }
    guest_ssh "cd /root/sdlop && cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo > /tmp/cmake.log 2>&1 && cmake --build build -j$(nproc) > /tmp/build.log 2>&1" \
        || { guest_ssh "tail -30 /tmp/build.log /tmp/cmake.log"; cleanup_vm "$qmp"; return 1; }
    echo "  build ok in guest"
    guest_ssh "pkill -f sdlop || true" >/dev/null 2>&1
    if [ "${SDLOP_QEMU_KEEP_VM:-0}" = "1" ]; then
        echo "  keeping deploy VM running (SDLOP_QEMU_KEEP_VM=1), ssh with:"
        echo "    sshpass -proot ssh -o StrictHostKeyChecking=no -p 2222 root@localhost"
    else
        cleanup_vm "$qmp"
    fi
    return 0
}

go_stop() { guest_ssh "poweroff" >/dev/null 2>&1; }

cleanup_vm() { # $1 = qmp socket path; always releases hostfwd :2222
    local qmp="$1"
    guest_ssh "poweroff" >/dev/null 2>&1 || true
    sleep 3
    # one last resort: the QMP socket path only appears in that QEMU's argv
    pkill -f "$qmp" 2>/dev/null || true
}

# Boot a scenario, run the in-guest checks. Prints scenario result lines.
# SDLOP_QEMU_EXTRA_ENV (if set) is appended to every scenario's guest env -
# used for the atomic-vs-legacy A/B comparison.
go_scenario() {
    local name="$1"; shift
    local vga_args="$1"; shift
    local guest_env="$1"; shift || true
    if [ -n "${SDLOP_QEMU_EXTRA_ENV:-}" ]; then
        guest_env="${guest_env:+$guest_env }$SDLOP_QEMU_EXTRA_ENV"
    fi
    local screendump_device="${1:-}"
    local qemu_display="${2:-none}"
    if [ "$qemu_display" = "gtk-gl" ]; then
        # virgl needs an X/GL stack; QEMU's egl-headless wants a /dev/dri
        # render node we do not have, so give it an Xvfb display instead
        if ! command -v Xvfb >/dev/null; then
            echo "RESULT $name FAIL (Xvfb not installed; needed for virgl)" >&2
            return 1
        fi
        if [ -z "${DISPLAY:-}" ]; then
            # start (or reuse) a persistent Xvfb on :99; if one is already
            # active the new instance exits immediately with 'server already
            # active' and we use the existing one
            Xvfb :99 -screen 0 640x480x24 >"$LOGS/xvfb.log" 2>&1 &
            sleep 1
            export DISPLAY=:99
        fi
    fi
    local qmp="$STATE/qmp-$name.sock"
    rm -f "$qmp"
    echo "== scenario $name ($vga_args${guest_env:+; env: $guest_env}) =="
    # shellcheck disable=SC2046,SC2086
    $(qemu_cmd "$qemu_display") $(qemu_common "$name" "$qemu_display" | tr '\n' ' ') \
        $vga_args -monitor none -qmp unix:"$qmp",server=on,wait=off -daemonize || { echo "RESULT $name FAIL (qemu start)"; return 1; }
    if ! wait_for_ssh "$name" 240; then
        echo "RESULT $name FAIL (no ssh)" >&2
        cleanup_vm "$qmp"
        return 1
    fi

    echo "  -- /dev/dri + modetest --"
    guest_ssh bash -s <<'REMOTE' | tee "$LOGS/$name-modetest.txt"
ls -l /dev/dri 2>&1 || true
shopt -s nullglob
for m in /sys/class/drm/card[0-9]*; do
    c="${m##*/}"
    case "$c" in
        *-*) continue ;;   # connector symlinks, not cards
    esac
    sys="$m/device"
    bus="$(awk -F= '/^PCI_SLOT_NAME=/{print $2}' "$sys/uevent" 2>/dev/null || true)"
    drv="$(basename "$(readlink -f "$sys/driver" 2>/dev/null)" 2>/dev/null || true)"
    printf '== %s (driver=%s pci=%s) ==\n' "$c" "${drv:-?}" "${bus:-?}"
    if [ -n "$bus" ]; then
        modetest -D "pci:$bus" -c
        echo '-- CRTCs/planes --'
        modetest -D "pci:$bus" -p
    elif [ -n "$drv" ]; then
        modetest -M "$drv" -c
    else
        echo 'no bus/driver info'
    fi
done
REMOTE

    echo "  -- test_kmsdrm (env: ${guest_env:-none}) --"
    guest_ssh "${guest_env:+$guest_env} /root/sdlop/build/test_kmsdrm" | tee "$LOGS/$name-test.txt"
    local test_rc=${PIPESTATUS[0]}

    # screendump check with a held banded frame (software path)
    if [[ "$name" != "bad-device" ]]; then
        local screendump_args=()
        if [ -n "$screendump_device" ]; then
            screendump_args=(--device "$screendump_device")
        fi
        guest_ssh "nohup env ${guest_env:+$guest_env} SDLOP_TEST_KMSDRM_HOLD=25 /root/sdlop/build/test_kmsdrm >/root/hold.log 2>&1 &" >/dev/null 2>&1
        sleep 6
        local ok=1
        if [ "$qemu_display" = "gtk-gl" ]; then
            # virgl scanout is not observable here: QMP screendump reports
            # 'no surface' on virtio-gpu-gl and the gtk window only shows the
            # primary (stdvga) display, which the driver never programs in the
            # SDLOP_KMSDRM_DEVICE=1 setup. test_kmsdrm's in-guest checks are the
            # verification for the virgl path.
            echo "  screendump: SKIP (virgl scanout not observable from host; see test output)"
        else
        for attempt in $(seq 1 12); do
            if [ ${#screendump_args[@]} -gt 0 ]; then
                python3 "$QMP_PY" --check-bands "${screendump_args[@]}" "$qmp" "$PPM/$name.ppm" && { ok=0; break; }
            else
                python3 "$QMP_PY" --check-bands "$qmp" "$PPM/$name.ppm" && { ok=0; break; }
            fi
            sleep 1
        done
        guest_ssh "pkill -f test_kmsdrm || true" >/dev/null 2>&1
        [ $ok -eq 0 ] && echo "  screendump: band pattern verified ($PPM/$name.ppm)" \
                      || echo "  screendump: NO band pattern (see $PPM/$name.ppm)"
        fi
        # GL path: hold a magenta GL frame and verify it reaches the scanout
        guest_ssh "nohup env ${guest_env:+$guest_env} SDLOP_TEST_KMSDRM_HOLD_GL=25 /root/sdlop/build/test_kmsdrm >/root/hold_gl.log 2>&1 &" >/dev/null 2>&1
        sleep 6
        local gl_ok=1
        if [ "$qemu_display" = "gtk-gl" ]; then
            echo "  screendump: SKIP GL frame (virgl scanout not observable from host)"
        elif [[ "$name" == virtio || "$name" == no-flip ]]; then
            # virtio-gpu without virgl has no 3D (dmesg: '-virgl', cap sets: 0);
            # GBM bos are CPU-mappable dumb buffers on bochs-drm/stdvga, so GL
            # scanout is verified there instead
            echo "  screendump: SKIP GL frame (virtio-gpu 2D has no 3D; GL scanout verified on bochs/stdvga)"
        else
        for attempt in $(seq 1 8); do
            if [ -n "$screendump_device" ]; then
                python3 "$QMP_PY" --device "$screendump_device" "$qmp" "$PPM/$name-gl.ppm" >/dev/null 2>&1
            else
                python3 "$QMP_PY" "$qmp" "$PPM/$name-gl.ppm" >/dev/null 2>&1
            fi
            if python3 "$REPO_ROOT/tools/qemu/ppm_magentacheck.py" "$PPM/$name-gl.ppm" >/dev/null; then
                gl_ok=0
                break
            fi
            sleep 1
        done
        if [ $gl_ok -eq 0 ]; then
            python3 "$REPO_ROOT/tools/qemu/ppm_magentacheck.py" "$PPM/$name-gl.ppm"
            echo "  screendump: GL magenta frame verified ($PPM/$name-gl.ppm)"
        else
            echo "  screendump: GL frame NOT magenta after retries ($PPM/$name-gl.ppm)"
        fi
        fi
        guest_ssh "pkill -f test_kmsdrm || true" >/dev/null 2>&1
    fi

    guest_ssh "cp /root/hold.log /tmp/hold.log 2>/dev/null; tail -5 /tmp/hold.log 2>/dev/null" >/dev/null 2>&1
    cleanup_vm "$qmp"

    if [ "$test_rc" -eq 0 ]; then
        echo "RESULT $name PASS"
        return 0
    fi
    echo "RESULT $name FAIL (test_kmsdrm exit $test_rc)"
    return 1
}

go_all() {
    go_deploy || return 1
    local failed=()
    go_scenario stdvga      "-vga std" "" "" || failed+=(stdvga)
    go_scenario bochs       "-vga std -device bochs-display,id=bochs0" "SDLOP_KMSDRM_DEVICE=1" "bochs0" || failed+=(bochs)
    go_scenario virtio      "-vga std -device virtio-gpu-pci,id=gpu0" "SDLOP_KMSDRM_DEVICE=1" "gpu0" || failed+=(virtio)
    go_scenario virtio-gl   "-vga std -device virtio-gpu-gl-pci,id=gpu0" "SDLOP_KMSDRM_DEVICE=1" "gpu0" "gtk-gl" || failed+=(virtio-gl)
    go_scenario no-flip     "-vga std -device virtio-gpu-pci,id=gpu0" "SDLOP_KMSDRM_DEVICE=1 SDLOP_KMSDRM_NOFLIP=1" "gpu0" || failed+=(no-flip)
    go_scenario dual-head   "-vga std -device bochs-display,id=bochs0" "" "" || failed+=(dual-head)
    go_scenario card-select "-vga std -device bochs-display,id=bochs0" "SDLOP_KMSDRM_DEVICE=1" "bochs0" || failed+=(card-select)
    go_scenario bad-device  "-vga std" "SDLOP_KMSDRM_DEVICE=99" || failed+=(bad-device)
    echo
    echo "======================================="
    if [ ${#failed[@]} -eq 0 ]; then
        echo "ALL SCENARIOS PASS"
    else
        echo "FAILED SCENARIOS: ${failed[*]}"
    fi
    return ${#failed[@]}
}

case "${1:-}" in
    install)    go_install ;;
    deploy)     go_deploy ;;
    scenario)   shift
                case "$1" in
                    stdvga)      go_scenario stdvga      "-vga std" "" "" ;;
                    bochs)       go_scenario bochs       "-vga std -device bochs-display,id=bochs0" "SDLOP_KMSDRM_DEVICE=1" "bochs0" ;;
                    virtio)      go_scenario virtio      "-vga std -device virtio-gpu-pci,id=gpu0" "SDLOP_KMSDRM_DEVICE=1" "gpu0" ;;
                    virtio-gl)   go_scenario virtio-gl   "-vga std -device virtio-gpu-gl-pci,id=gpu0" "SDLOP_KMSDRM_DEVICE=1" "gpu0" "gtk-gl" ;;
                    no-flip)     go_scenario no-flip     "-vga std -device virtio-gpu-pci,id=gpu0" "SDLOP_KMSDRM_DEVICE=1 SDLOP_KMSDRM_NOFLIP=1" "gpu0" ;;
                    dual-head)   go_scenario dual-head   "-vga std -device bochs-display,id=bochs0" "" "" ;;
                    card-select) go_scenario card-select "-vga std -device bochs-display,id=bochs0" "SDLOP_KMSDRM_DEVICE=1" "bochs0" ;;
                    bad-device)  go_scenario bad-device  "-vga std" "SDLOP_KMSDRM_DEVICE=99" ;;
                    *) echo "unknown scenario: $1" >&2; exit 2 ;;
                esac ;;
    all)        go_all ;;
    *)          echo "usage: $0 {install|deploy|scenario <name>|all}" >&2; exit 2 ;;
esac
