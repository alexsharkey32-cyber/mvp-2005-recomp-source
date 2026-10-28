# Making a release

Two GitHub releases make up what players download:

* **`vX.Y`**: the game for macOS, Windows and Linux. Pushing a tag `vX.Y` runs `.github/workflows/build.yml`,
  which builds all three (`tools/release/package_{mac,windows,linux}.sh`) and attaches them to the release.
* **`features`**: the optional downloads the launcher's Setup offers, with `manifest.json`. The launcher reads
  `https://github.com/<FEATURES_REPO>/releases/download/features/manifest.json` (built into the program by the
  workflow; `features_url = ...` in mvp2005.ini or `MVP_FEATURES_URL` override it).

The repository holds `generated/` (the recompiled game code) so the cloud builds don't need the disc. Keep it
private; the `features` release has to be readable by players, so put it in a public repository of its own and set
the Actions variable `FEATURES_REPO` to it (or make one repository public, your call).

## The features release

On the machine with the data (`mods/`, `textures/replace`, `build/hd-2x`):

```
python3 tools/release/make_hd_2x.py textures/replace build/hd-2x          # the 2x pack from the 4x one (~15 min)
gh release create features --repo OWNER/REPO --title Features --notes "Optional downloads for the launcher"
python3 tools/release/make_features.py build/features --upload OWNER/REPO --tag features
```

`make_features.py` packs each feature listed in `tools/release/features.json` (eras are found from `mods/pack-*`)
into plain `.tar` parts under 1.9 GB (GitHub's limit is 2 GB per file), records each part's size and SHA-256 in
`manifest.json`, uploads each part as soon as it is made and deletes it, so the 44 GB HD pack never needs 44 GB of
free space twice. Run it again with `--only id,id` to update some features (the others stay as they are in the
manifest); bump `--version` so players who have the old one see "UPDATE". Previews come from
`build/features/previews/*.png` (uploaded as `preview-<name>.png`).

Without `--upload` the folder is an offline install: put it next to the game as `features/` (or point
`MVP_FEATURES_DIR` at it) and Setup installs from it without downloading.

## Checking a build

* `mvp2005 --features` lists the features and what is installed; `--features install hd-2x` installs one.
* `MVP_GUI_PAGE=setup:0|setup:1 MVP_GUI_SHOT=x.png mvp2005 --launcher` screenshots the setup's steps.
* The first start on a clean machine: delete the game folder (or set `MVP_HOME` to an empty one).
