#!/bin/bash
# Publishes the game builds of the latest successful CI run as a release in the public releases repository.
# Log: tools/mac/publish_game.log
cd "$(dirname "$0")/../.."
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
SRC=alexsharkey32-cyber/mvp2005-recomp; DST=alexsharkey32-cyber/mvp2005-recomp-releases; TAG="v$(sed -n 's/^#define MVP_VERSION "\(.*\)"/\1/p' runtime/include/version.h)"   # from runtime/include/version.h: raise it for a new release
{
  echo "== publish game $(date)"
  RUN=$(gh run list --repo "$SRC" --workflow build --branch main --status success --limit 1 --json databaseId --jq '.[0].databaseId')
  echo "run $RUN"
  rm -rf build/ci && mkdir -p build/ci && gh run download "$RUN" --repo "$SRC" -D build/ci && ls -R build/ci
  for f in build/ci/*/*; do b=$(basename "$f"); n=$(echo "$b" | sed -E "s/-(dev|main)-/-$TAG-/"); mv "$f" "build/ci/$n"; done
  NOTES="Updating: download the one for your computer and replace the old app or folder. Your settings, saves and installed features stay where they are.

Unzip the one for your computer and start MVP Baseball 2005. You need your own MVP Baseball 2005 disc, USA version (SLUS-21135), as a .iso. The first start checks the disc and lets you pick the features to download (today's rosters, portraits, seasons from history, HD textures).

- Mac: Apple Silicon, macOS 11 or later. The first time, right-click the app and choose Open.
- Windows (beta): Windows 10 1803 or later, 64-bit. If SmartScreen warns, choose More info > Run anyway.
- Linux (beta): 64-bit, 2022 or later distributions."
  gh release view "$TAG" --repo "$DST" >/dev/null 2>&1 && gh release upload "$TAG" build/ci/*.zip build/ci/*.tar.gz --repo "$DST" --clobber \
    || gh release create "$TAG" build/ci/*.zip build/ci/*.tar.gz --repo "$DST" --title "MVP Baseball 2005 Recomp $TAG" --notes "$NOTES"
  echo "== exit $? $(date)"
} > tools/mac/publish_game.log 2>&1
exit 0
