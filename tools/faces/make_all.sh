#!/bin/bash
# Faces and portraits for every league, in one go (hours; can be stopped and started again, it picks up where it was):
#   tools/faces/make_all.sh "game/MVP Baseball 2005 (USA).iso" [mods/pack-1977 ...]
# Needs Python 3 with numpy, Pillow, opencv-python and onnxruntime, and a C compiler for the portrait rasterizer.
# Without league names: every era pack (the modern rosters' portraits ship made: their real-head players come from
# in-game captures, tools/portraits/capture.py, merged with these renders). One-time inputs (tools/faces/facegen.py prepare,
# tools/faces/upscale_sources.py) go to faces_work/, the head templates to faces_work/templates (recorded from the
# game: tools/faces/record_templates.py with a studio run, see research/notes/findings.md).
set -e
ISO="$1"; shift
W=faces_work
LEAGUES="$*"; [ -z "$LEAGUES" ] && LEAGUES="$(ls -d mods/pack-*/ | grep -v custom | sed 's#/$##')"
[ -f tools/portraits/csrc/libraster.so ] || cc -O3 -shared -fPIC -o tools/portraits/csrc/libraster.so tools/portraits/csrc/raster.c -lm
[ -f $W/faces.npz ] || python3 tools/faces/facegen.py prepare "$ISO" $W
[ -f $W/faces_hd.npz ] || python3 tools/faces/upscale_sources.py $W "${ESRGAN:-hd/realesr-general-x4v3.onnx}"
for M in $LEAGUES; do
  if [ ! -f $M/faces.txt ]; then
    python3 tools/faces/build_faces.py "$ISO" $M $W --part 0/1 && python3 tools/faces/build_faces.py "$ISO" $M $W --merge
  fi
  OUT=$M/portraits; EXTRA=""
  if [ ! -f $OUT/.rendered ]; then
    R=$W/render_$(basename $M)
    python3 tools/portraits/render_league.py "$ISO" $M $W/templates $W $R $EXTRA
    python3 tools/portraits/build.py "$ISO" $R $OUT && touch $OUT/.rendered
  fi
done
