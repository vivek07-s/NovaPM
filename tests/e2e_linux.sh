#!/bin/sh
# End-to-end test of the threshold guard on real processes (Linux).
#   usage: e2e_linux.sh /path/to/novapm-cli /path/to/load_gen
# Phase 1: a 100 % CPU load must be alerted and then suspended.
# Phase 2: a ~70 % CPU load must only be alerted, never suspended.
# The loads run one after the other so that each one has a core to itself, even on a 1-core machine.
CLI="$1"; LOAD="$2"
DB="$(mktemp -u /tmp/novapm_e2e_XXXXXX).db"
export NOVAPM_CONFIG=/nonexistent/novapm.ini      # built-in defaults: alert 60 / stop 80
PIDS=""
cleanup() { for p in $PIDS; do kill -CONT "$p" 2>/dev/null; kill "$p" 2>/dev/null; done; wait 2>/dev/null; rm -f "$DB" "$DB"-wal "$DB"-shm; }
fail() { echo "FAIL: $1"; cleanup; exit 1; }
state() { awk '{print $3}' "/proc/$1/stat" 2>/dev/null; }
count() { "$CLI" query "$1" --db "$DB" | sed -n 3p | tr -d ' '; }

# ---- phase 1: 100 % load -> alert, then suspend
"$LOAD" 100 60 & A=$!; PIDS="$PIDS $A"
sleep 1
"$CLI" watch --db "$DB" --duration 8 --interval 500 --warn 60 --stop 80 --sustain 3 || fail "watch (phase 1) exited with an error"
[ "$(state $A)" = "T" ] || fail "100% process was not suspended (state=$(state $A))"
[ "$(count "SELECT COUNT(*) FROM alerts WHERE pid=$A AND action='warn'")" -ge 1 ] 2>/dev/null || fail "no alert logged for the 100% process"
[ "$(count "SELECT COUNT(*) FROM alerts WHERE pid=$A AND action='stop' AND result='suspended'")" = "1" ] || fail "expected exactly 1 logged stop for the 100% process"
[ "$(count "SELECT COUNT(*) FROM events WHERE pid=$A AND event='auto_stop'")" = "1" ] || fail "auto_stop event missing from the events table"
"$CLI" signal "$A" cont --db "$DB" >/dev/null
sleep 0.3
[ "$(state $A)" != "T" ] || fail "process still stopped after 'signal cont'"
kill "$A" 2>/dev/null; wait "$A" 2>/dev/null

# ---- phase 2: ~70 % load -> alert only
"$LOAD" 70 60 & B=$!; PIDS="$PIDS $B"
sleep 1
"$CLI" watch --db "$DB" --duration 8 --interval 500 --warn 60 --stop 80 --sustain 3 || fail "watch (phase 2) exited with an error"
[ "$(state $B)" != "T" ] || fail "70% process was suspended but must only be alerted"
[ "$(count "SELECT COUNT(*) FROM alerts WHERE pid=$B AND action='warn'")" -ge 1 ] 2>/dev/null || fail "no alert logged for the 70% process"
[ "$(count "SELECT COUNT(*) FROM alerts WHERE pid=$B AND action='stop'")" = "0" ] || fail "70% process must never be stopped"

# ---- phase 3: alert-only mode (--no-stop) never suspends, even at 100 %
kill "$B" 2>/dev/null; wait "$B" 2>/dev/null
"$LOAD" 100 60 & C=$!; PIDS="$PIDS $C"
sleep 1
"$CLI" watch --db "$DB" --duration 6 --interval 500 --no-stop --quiet || fail "watch (phase 3) exited with an error"
[ "$(state $C)" != "T" ] || fail "--no-stop must never suspend a process"

cleanup
echo "PASS: 100% load alerted + suspended + logged, resume works; 70% load alerted only; --no-stop never suspends"
