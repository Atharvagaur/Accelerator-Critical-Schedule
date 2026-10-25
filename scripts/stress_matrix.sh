#!/usr/bin/env bash
# Liveness/fairness stress matrix for accel_critical.
#
# For each worker count, loads the scheduler, runs stress-ng for DURATION seconds,
# stops the scheduler, and records: scheduler state before/after, sched_ext kernel
# messages emitted during the run, and the loader log. Requires root and stress-ng.
#
# Usage:  sudo WORKERS="2 4 8" DURATION=60 ./scripts/stress_matrix.sh
# Output: $OUT (default /tmp/opencode/stress-<timestamp>)

set -u

LOADER=${LOADER:-$(dirname "$0")/../accel_critical}
LOADER=$(realpath "$LOADER")
WORKERS=${WORKERS:-"8 16 32"}
DURATION=${DURATION:-30}
OUT=${OUT:-/tmp/opencode/stress-$(date +%Y%m%d-%H%M%S)}
STATE=/sys/kernel/sched_ext/state
OPS=/sys/kernel/sched_ext/root/ops

if [ "$(id -u)" -ne 0 ]; then
	echo "error: must run as root (loads the scheduler)" >&2
	exit 1
fi
for tool in stress-ng dmesg; do
	command -v "$tool" >/dev/null || { echo "error: $tool not found" >&2; exit 1; }
done
[ -x "$LOADER" ] || { echo "error: loader not built: $LOADER" >&2; exit 1; }

mkdir -p "$OUT"
LOADER_PID=""
cleanup() {
	if [ -n "$LOADER_PID" ] && kill -0 "$LOADER_PID" 2>/dev/null; then
		kill -INT "$LOADER_PID" 2>/dev/null
		wait "$LOADER_PID" 2>/dev/null
	fi
}
trap cleanup EXIT INT TERM

sched_state() { cat "$STATE" 2>/dev/null || echo "unavailable"; }
sched_ops()   { cat "$OPS" 2>/dev/null || echo "none"; }

summary="$OUT/summary.txt"
printf "%-8s %-12s %-12s %-10s %-8s\n" workers state_before state_after ops_after kernel_msgs > "$summary"

for n in $WORKERS; do
	run="$OUT/w$n"
	mkdir -p "$run"
	before_state=$(sched_state)

	# The loader writes metrics.csv into its working directory.
	(cd "$run" && exec "$LOADER" --csv) > "$run/loader.log" 2>&1 &
	LOADER_PID=$!
	sleep 3

	dmesg_mark=$(dmesg | wc -l)
	stress-ng --cpu "$n" --timeout "${DURATION}s" --metrics-brief > "$run/stress.log" 2>&1
	stress_rc=$?

	after_state=$(sched_state)
	ops_after=$(sched_ops)
	dmesg | tail -n +"$((dmesg_mark + 1))" | grep -i "sched_ext" > "$run/kernel.log"
	kernel_msgs=$(wc -l < "$run/kernel.log")

	cleanup
	LOADER_PID=""

	printf "%-8s %-12s %-12s %-10s %-8s\n" "$n" "$before_state" "$after_state" "$ops_after" "$kernel_msgs" >> "$summary"
	echo "workers=$n stress_rc=$stress_rc state=$after_state kernel_msgs=$kernel_msgs"
done

echo "Results in $OUT"
cat "$summary"
