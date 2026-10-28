#!/bin/bash
# Signs this Mac in to GitHub once (for uploading the downloads). Installs the GitHub CLI if needed, then opens
# your browser: copy the one-time code it shows into the page and approve.
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
command -v gh >/dev/null || brew install gh
gh auth status >/dev/null 2>&1 && { echo "Already signed in:"; gh auth status; } || gh auth login --hostname github.com --git-protocol https --web --scopes repo
gh auth status > "$(dirname "$0")/github_login.log" 2>&1
echo; echo "Done. You can close this window."
