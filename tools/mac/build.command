#!/bin/bash
# Builds the game natively on this Mac (double-click in Finder). Output: tools/mac/build.log
cd "$(dirname "$0")/../.."
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
{
  echo "== build $(date)"
  make -f Makefile -f tools/mac/gfx.mk generated/.stamp && \
  make -j8 -f Makefile -f tools/mac/gfx.mk build/mvp2005
  echo "== exit $?"
} > tools/mac/build.log 2>&1
[ -f tools/mac/.autorun ] && grep -q "== exit 0" tools/mac/build.log && bash tools/mac/run.command
exit 0
