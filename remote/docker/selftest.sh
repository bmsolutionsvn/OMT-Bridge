#!/bin/sh
# Runs inside the image: relay + one 10 Mbps bench through it. Exits 0 only when the
# receiver's summary says PASS.
set -eu

TOKEN="selftest-$$"
TMP="${TMPDIR:-/tmp}"

bmb-relay --port 7000 --token "$TOKEN" &
RELAY=$!
trap 'kill $RELAY 2>/dev/null || true' EXIT
sleep 1

srt-bench recv --host 127.0.0.1 --port 7000 \
  --streamid "#!::r=bmb/selftest/relay/cam1,m=request,u=$TOKEN" \
  --seconds 5 > "$TMP/recv.log" &
RECV=$!
sleep 1

srt-bench send --host 127.0.0.1 --port 7000 \
  --streamid "#!::r=bmb/selftest/relay/cam1,m=publish,u=$TOKEN" \
  --mbps 10 --seconds 5 > "$TMP/send.log"

wait "$RECV" || true
cat "$TMP/send.log" "$TMP/recv.log"
grep -q '| PASS |' "$TMP/recv.log"