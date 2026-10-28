#!/bin/bash
# Builds the rest of the HD texture pack natively on this Mac (double-click in Finder; leave the window open).
# Resumable: run it again to continue. Progress: tools/mac/hdpack.log
cd "$(dirname "$0")/../.."
export PATH="/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:$PATH"
LOG=tools/mac/hdpack.log
{
  echo "== hdpack $(date)"
  if [ ! -x tools/mac/venv/bin/python3 ]; then
    python3 -m venv tools/mac/venv && tools/mac/venv/bin/pip install -q --upgrade pip && tools/mac/venv/bin/pip install -q numpy pillow onnxruntime
  fi
  PY=tools/mac/venv/bin/python3
  $PY -c "import numpy, PIL, onnxruntime; print('python ok, onnxruntime', onnxruntime.__version__)" || exit 1
  # 1. the original textures (sources) and the index, read from the disc
  if [ ! -f game/hdpack/.src_complete ]; then
    $PY tools/hd/extract_textures.py "game/MVP Baseball 2005 (USA).iso" game/hdpack mods && touch game/hdpack/.src_complete
  fi
  echo "== $(ls game/hdpack/src | wc -l) sources, $(wc -l < game/hdpack/index.tsv) in the index"
  # 2. upscale: menu art first, then tiers 1-4 in order; 5 processes x 2 threads
  for i in 0 1 2 3 4; do
    caffeinate -i $PY tools/hd/build_pack.py game/hdpack ~/Downloads textures/replace --tiers 1-4 --order game/hdpack/order_menu.txt --quick --part $i/5 --threads 2 > tools/mac/hdpack_$i.log 2>&1 &
  done
  wait
  tail -n 2 tools/mac/hdpack_*.log
  echo "== done $(date): $(ls textures/replace | wc -l) textures in the pack"
} >> $LOG 2>&1
exit 0
