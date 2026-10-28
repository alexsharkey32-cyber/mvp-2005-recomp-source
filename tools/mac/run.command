#!/bin/bash
# Test runs on this Mac (double-click in Finder). tools/mac/run.env sets SECS, RUN_ARGS and RUN_ENV (RUN_ENV2, RUN_ENV3: more runs)
cd "$(dirname "$0")/../.."
SECS=60
source tools/mac/run.env
rm -rf tools/mac/shots; mkdir -p tools/mac/shots
: > tools/mac/run.log
n=0
for E in "$RUN_ENV" "$RUN_ENV2" "$RUN_ENV3"; do
  n=$((n+1)); [ -z "$E" ] && continue
  A="$RUN_ARGS"; [ $n -eq 2 ] && [ -n "$RUN_ARGS2" ] && A="$RUN_ARGS2"; [ $n -eq 3 ] && [ -n "$RUN_ARGS3" ] && A="$RUN_ARGS3"
  cp tools/mac/test.ini tools/mac/run.ini
  rm -rf tools/mac/mc_run; mkdir -p tools/mac/mc_run; cp -R memcards tools/mac/mc_run/   # same memory cards every run (a copy of the game's)
  {
    echo "== run $(date) :: $E"
    t0=$(date +%s)
    env MVP_CONFIG=tools/mac/run.ini MVP_MEMCARDS=tools/mac/mc_run/memcards $E ./build/mvp2005 $A --iso "game/MVP Baseball 2005 (USA).iso" &
    pid=$!
    for i in $(seq 1 $SECS); do sleep 1; kill -0 $pid 2>/dev/null || break; done
    kill $pid 2>/dev/null; sleep 1; kill -9 $pid 2>/dev/null
    wait $pid; rc=$?; echo "== exit $rc after $(( $(date +%s) - t0 ))s"
    if [ $rc -ne 143 ] && [ $rc -ne 0 ]; then sleep 3; cr=$(ls -t ~/Library/Logs/DiagnosticReports/mvp2005* 2>/dev/null | head -1); [ -n "$cr" ] && cp "$cr" tools/mac/crash.ips && echo "== crash report: tools/mac/crash.ips"; fi
  } >> tools/mac/run.log 2>&1
done
exit 0
