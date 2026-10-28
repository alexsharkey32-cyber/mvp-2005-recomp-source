#!/bin/bash
# Makes the release download for Macs (dist/MVPBaseball2005-*-mac.zip) from build/mvp2005. Log: tools/mac/package.log
cd "$(dirname "$0")/../.."
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
bash tools/release/package_mac.sh build/mvp2005 dist > tools/mac/package.log 2>&1
echo "== exit $?" >> tools/mac/package.log
exit 0
