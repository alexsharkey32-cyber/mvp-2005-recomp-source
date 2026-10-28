#!/bin/bash
# Makes the 2x HD pack (build/hd-2x) from textures/replace. Double-click in Finder; resumable. Log: tools/mac/hd2x.log
cd "$(dirname "$0")/../.."
caffeinate -i tools/mac/venv/bin/python3 tools/release/make_hd_2x.py textures/replace build/hd-2x --jobs 9 > tools/mac/hd2x.log 2>&1
exit 0
