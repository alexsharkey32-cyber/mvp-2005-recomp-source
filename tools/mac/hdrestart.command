#!/bin/bash
# Restarts the HD pack build (picks up a changed hdpack.command). Progress: tools/mac/hdpack.log
cd "$(dirname "$0")/../.."
pkill -f "tools/hd/build_pack.py"; sleep 2
bash tools/mac/hdpack.command
exit 0
