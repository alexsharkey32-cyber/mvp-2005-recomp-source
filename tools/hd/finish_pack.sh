#!/bin/bash
# Builds (or continues) the HD texture pack in textures/replace/. Run it from anywhere; stop it with Ctrl+C whenever
# you like and run it again later: finished textures are skipped.
#
#   tools/hd/finish_pack.sh [folder with the Real-ESRGAN .pth models] [tiers, default 1-3] [workers]
#
# Tiers: 1 players' uniforms and equipment, 2 heads, ballparks, names and numbers, 3 alternates, throwbacks and the
# menus (1-3 is about 10 GB on disk), 4 minor leagues and the 2005 players' own faces (+7 GB), 5 the rest (+6 GB).
#
# Needs python3 with numpy and Pillow; `pip3 install onnxruntime` makes it 3-4 times faster.
# Pass 1 uses the fast model for everything (the whole game looks sharp after it), pass 2 redoes the players'
# uniforms, heads and faces with the best model. Textures made by an older version of these tools are brought up
# to date first (a quick repair of their colours; menu art and fonts are made again).
cd "$(dirname "$0")/../.." || exit 1
MODELS=${1:-$HOME/Downloads}; TIERS=${2:-1-3}
N=${3:-$(( ($(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4) + 1) / 2 ))}
ISO=$(ls game/*.iso 2>/dev/null | head -1)
python3 -c "import numpy, PIL" 2>/dev/null || { echo "This needs numpy and Pillow:  pip3 install numpy pillow"; exit 1; }
python3 -c "import onnxruntime" 2>/dev/null || echo "Tip: 'pip3 install onnxruntime' makes this 3-4 times faster (stop with Ctrl+C, install, run again)."
[ -f "$MODELS/realesr-general-x4v3.pth" ] || { echo "No Real-ESRGAN models in $MODELS (realesr-general-x4v3.pth, RealESRGAN_x4plus.pth, RealESRGAN_x4plus_anime_6B.pth)"; exit 1; }
W=game/hdpack
if [ ! -f $W/src/.complete ] || [ ! -f $W/index.tsv ]; then
    [ -n "$ISO" ] || { echo "No disc image in game/"; exit 1; }
    echo "Reading the textures out of the disc (a few minutes)..."
    python3 tools/hd/extract_textures.py "$ISO" $W mods || exit 1
fi
python3 tools/hd/build_pack.py $W "$MODELS" textures/replace --prune
DUMPS=""; for d in $W/dumps textures/dump; do [ -d $d ] && DUMPS="${DUMPS:+$DUMPS,}$d"; done
run() {   # run "label" args...: N workers side by side
    echo "== $1"; shift
    for i in $(seq 0 $((N - 1))); do
        python3 tools/hd/build_pack.py $W "$MODELS" textures/replace --part $i/$N --threads 2 "$@" &
    done
    wait
}
trap 'kill 0' INT
run "pass 1 of 2: everything in tiers $TIERS, fast model" --tiers $TIERS --quick ${DUMPS:+--dumps $DUMPS} --redo uniform,equipment,lettering,head,face,ballpark,menu,portrait,dump
run "pass 2 of 2: players, heads and faces with the best model" --tiers 1-2 --only uniform,equipment,head,face
echo "Done: $(ls textures/replace | wc -l) textures in textures/replace"
