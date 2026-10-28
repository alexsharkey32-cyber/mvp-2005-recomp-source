#!/bin/bash
# A live progress bar for the feature upload (updates every 15 seconds). Close the window to stop watching;
# the upload itself keeps going.
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
REPO=alexsharkey32-cyber/mvp2005-recomp-releases
TOTAL=63444063917
start=""; t0=$(date +%s)
while true; do
  done_b=$(gh api "repos/$REPO/releases/tags/features" --jq '[.assets[]|select(.name|endswith(".tar"))|.size]|add // 0' 2>/dev/null)
  last=$(gh api "repos/$REPO/releases/tags/features" --jq '[.assets[]|select(.name|endswith(".tar"))]|sort_by(.created_at)|last|.name' 2>/dev/null)
  [ -z "$done_b" ] && { sleep 15; continue; }
  # the part being sent right now: what the uploading gh process has sent so far
  cur=0; pid=$(pgrep -f "gh release upload" | head -1)
  if [ -n "$pid" ]; then cur=$(nettop -P -L 1 -x -J bytes_out -p "$pid" 2>/dev/null | tail -1 | awk -F, '{print $2+0}'); cur=${cur:-0}; fi
  now_part=$(ps -o args= -p "$pid" 2>/dev/null | grep -o '[^ /]*\.tar' | head -1)
  done_b=$(( done_b + cur ))
  [ -z "$start" ] && start=$done_b
  pct=$(( done_b * 1000 / TOTAL )); w=40; fill=$(( done_b * w / TOTAL ))
  bar=$(printf "%${fill}s" | tr ' ' '#')$(printf "%$((w - fill))s" | tr ' ' '.')
  el=$(( $(date +%s) - t0 )); rate=0; eta="calculating"
  if [ $el -gt 120 ] && [ $done_b -gt $start ]; then rate=$(( (done_b - start) / el )); left=$(( (TOTAL - done_b) / rate )); eta="$((left / 3600)) h $(( (left % 3600) / 60 )) min left"; fi
  clear
  echo "MVP Baseball 2005: uploading the features"
  echo
  printf "  [%s] %d.%d%%\n" "$bar" $((pct / 10)) $((pct % 10))
  printf "  %.1f GB of %.1f GB   %s\n" "$(echo "$done_b/1000000000" | bc -l)" "$(echo "$TOTAL/1000000000" | bc -l)" "$eta"
  echo
  echo "  last finished: $last"
  [ -n "$now_part" ] && echo "  sending now:   $now_part ($(( cur / 1000000 )) MB so far)"
  echo "  (updates every 15 seconds; closing this window doesn't stop the upload)"
  [ "$done_b" -ge "$TOTAL" ] && { echo; echo "  All done."; break; }
  sleep 15
done
