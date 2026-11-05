#!/usr/bin/env bash
# End-to-end test: starts Cinder and talks to it with the real redis-cli.
# Usage: bash tests/integration_test.sh   (after building into ./build)
set -euo pipefail
cd "$(dirname "$0")/.."

PORT=${PORT:-6390}
./build/cinder "$PORT" > /dev/null &
PID=$!
trap 'kill $PID 2>/dev/null || true' EXIT
sleep 0.3

cli() { redis-cli -p "$PORT" "$@"; }
fail=0
expect() {  # expect <expected> <command...>
    local want="$1"; shift
    local got; got="$(cli "$@")"
    if [[ "$got" != "$want" ]]; then
        echo "FAIL: $* -> got '$got', want '$want'"; fail=1
    fi
}

expect "PONG" PING
expect "OK"   SET greeting hello
expect "hello" GET greeting
expect "1"    EXPIRE greeting 1
expect "1"    TTL greeting
sleep 1.2
expect ""     GET greeting          # expired (redis-cli prints nil as empty)
expect "0"    EXISTS greeting
expect "OK"   SET a 1
expect "1"    DEL a b

# Pipelining: 1000 commands sent in one go must all get replies.
replies=$(for i in $(seq 1 1000); do echo "SET key:$i $i"; done | cli --pipe 2>&1 | grep -o 'replies: [0-9]*')
[[ "$replies" == "replies: 1000" ]] || { echo "FAIL: pipeline -> $replies"; fail=1; }
expect "1000" DBSIZE

# 50 clients at once.
for i in $(seq 1 50); do cli SET "parallel:$i" "$i" > /dev/null & done
wait $(jobs -p | grep -v "^$PID$") 2>/dev/null || true
expect "1050" DBSIZE

if [[ $fail -eq 0 ]]; then echo "integration tests passed"; else exit 1; fi
