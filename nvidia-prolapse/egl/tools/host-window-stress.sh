#!/usr/bin/env bash
# host-window stress: RUNS runs of test/host-window with one consumer type, JOBS in parallel. The pass bar is 0
# failed runs; a failure is never a rate (README "Tests", FENCE-CONTRACT.md §6a). Each run is a fresh process: a new
# window surface, first frame, steady state, swap interval 1 -> 0 -> 1, a resize, teardown.
#
#   tools/host-window-stress.sh --platform-lib out/host-amplify/libnvnative-egl-android.so --consumer vk-write \
#       --runs 5000 --jobs 6 [--process in|sep|mix] [--logdir DIR] [--front LIB] [--vk-hal LIB] [-- extra arguments]
# --front / --vk-hal: the libraries under test (default: this tree's out/host/libEGL_nvnative.so and
#            <driver checkout>/hal/out/host/vulkan.nvidia.so); point them at older builds to check that the harness still
#            detects D1. The driver checkout: ../../tools/driver_source.py path.
# --process: in = consumer thread in the producer's process, sep = exec'd consumer process (--consumer-process),
#            mix (default) = odd runs in-process, even runs separate process.
# Output: one summary line; the full log of every failed run in LOGDIR (default: a mktemp directory, printed).
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
driver=$(python3 "$here/../../tools/driver_source.py" path)
plat="" consumer="gl-write" runs=100 jobs=4 process=mix logdir="" extra=()
front=out/host/libEGL_nvnative.so vkhal=$driver/hal/out/host/vulkan.nvidia.so
while [ $# -gt 0 ]; do
    case "$1" in
    --platform-lib) plat=$2; shift 2 ;;
    --consumer) consumer=$2; shift 2 ;;
    --front) front=$2; shift 2 ;;
    --vk-hal) vkhal=$2; shift 2 ;;
    --runs) runs=$2; shift 2 ;;
    --jobs) jobs=$2; shift 2 ;;
    --process) process=$2; shift 2 ;;
    --logdir) logdir=$2; shift 2 ;;
    --) shift; extra=("$@"); break ;;
    *) echo "host-window-stress: unknown argument $1" >&2; exit 2 ;;
    esac
done
[ -n "$plat" ] || { echo "host-window-stress: --platform-lib is required" >&2; exit 2; }
[ -n "$logdir" ] || logdir=$(mktemp -d /tmp/host-window-stress.XXXXXX)
mkdir -p "$logdir"
plat=$(realpath "$plat")
front=$(cd "$here" && realpath "$front")
vkhal=$(cd "$here" && realpath "$vkhal")
export HWS_HERE=$here HWS_DRIVER=$driver HWS_PLAT=$plat HWS_FRONT=$front HWS_VKHAL=$vkhal HWS_CONSUMER=$consumer HWS_PROCESS=$process HWS_LOGDIR=$logdir
export HWS_EXTRA="${extra[*]:-}"
export DISPLAY= WAYLAND_DISPLAY= LSAN_OPTIONS=suppressions=$here/test/lsan.supp

run_one() {
    local i=$1 proc=()
    case "$HWS_PROCESS" in
    sep) proc=(--consumer-process) ;;
    mix) [ $((i % 2)) -eq 0 ] && proc=(--consumer-process) ;;
    esac
    local out rc
    # shellcheck disable=SC2086
    out=$(cd "$HWS_HERE" && timeout 300 ./out/host/host-window --gralloc "$HWS_DRIVER"/gralloc/out/host/libgbm_mesa_wrapper.so \
        --front "$HWS_FRONT" --platform-lib "$HWS_PLAT" --consumer "$HWS_CONSUMER" \
        --vk-hal "$HWS_VKHAL" "${proc[@]}" $HWS_EXTRA 2>&1)
    rc=$?
    if [ $rc -ne 0 ] || ! grep -q "^host-window: PASS" <<<"$out"; then
        printf '%s\n' "$out" > "$HWS_LOGDIR/fail-$i.log"
        echo "FAIL ${proc[*]:-in-process}"
    else
        echo "PASS ${proc[*]:-in-process}"
    fi
}
export -f run_one
t0=$(date +%s)
summary=$(seq 1 "$runs" | xargs -P "$jobs" -I{} bash -c 'run_one {}' | sort | uniq -c | tr '\n' ';')
t1=$(date +%s)
nfail=$(find "$logdir" -name 'fail-*.log' | wc -l)
echo "host-window-stress: consumer $consumer, platform $(basename "$(dirname "$plat")")/$(basename "$plat"), front $front," \
     "HAL $vkhal, $runs runs" \
     "($process), $((t1 - t0)) s: $summary failed runs: $nfail (logs: $logdir)"
[ "$nfail" -eq 0 ]
