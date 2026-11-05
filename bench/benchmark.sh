#!/usr/bin/env bash
# Benchmarks Cinder with the official redis-benchmark tool.
# Usage: bash bench/benchmark.sh   (after building into ./build)
set -euo pipefail
cd "$(dirname "$0")/.."

PORT=${PORT:-6391}
./build/cinder "$PORT" > /dev/null &
PID=$!
trap 'kill $PID 2>/dev/null || true' EXIT
sleep 0.3

echo "== 50 clients, 1 command per round trip =="
redis-benchmark -p "$PORT" -t set,get -n 200000 -c 50 -q
echo "== 50 clients, 16 pipelined commands per round trip =="
redis-benchmark -p "$PORT" -t set,get -n 1000000 -c 50 -P 16 -q
