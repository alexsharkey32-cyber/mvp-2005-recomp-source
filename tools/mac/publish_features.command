#!/bin/bash
# Uploads the features (rosters, portraits, eras, HD packs) to the public releases repository, part by part.
# Resumable: run it again and it carries on (finished features are kept in build/features/manifest.json).
# Log: tools/mac/publish_features.log
cd "$(dirname "$0")/../.."
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
REPO=alexsharkey32-cyber/mvp2005-recomp-releases
{
  echo "== publish $(date)"
  gh release view features --repo "$REPO" >/dev/null 2>&1 || \
    gh release create features --repo "$REPO" --title "Features" --notes "Optional downloads for the game's Setup (today's rosters, portraits, seasons from history, HD textures). The game installs these for you: you don't need to download them by hand."
  mkdir -p build/features/previews && cp build/pv/hd_before.png build/pv/hd_after.png build/pv/portraits.png build/features/previews/ 2>/dev/null
  caffeinate -is tools/mac/venv/bin/python3 -u tools/release/make_features.py build/features --upload "$REPO" --tag features --skip-done
  echo "== exit $? $(date)"
} >> tools/mac/publish_features.log 2>&1
exit 0
