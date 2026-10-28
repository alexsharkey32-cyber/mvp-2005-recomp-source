// Features: the optional parts of the game (today's rosters, portraits, seasons from history, the HD texture pack...)
// listed in a manifest (features/manifest.json, fetched from the release page), each downloaded as one or more
// archives, checked against its SHA-256 and unpacked into the game folder. The launcher's setup shows them with
// their sizes and previews and lets the player pick what to install or remove.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct FeaturePart { std::string file, url, sha256; uint64_t size = 0; };
struct FeaturePreview { std::string file, url, caption, after_file, after_url; };    // after_*: a before/after pair
struct Feature {
    std::string id, name, group, desc, version;
    uint64_t install_size = 0, download_size = 0;
    bool def = false, required = false;
    std::vector<std::string> requires_, excludes, paths, remove;   // paths: there when installed; remove: deleted on removal (default paths)
    bool era = false; int season = 0;
    std::vector<FeaturePart> parts;
    std::vector<FeaturePreview> previews;
    std::vector<std::pair<std::string, int>> on_install, on_remove;      // settings to set
    // state
    bool installed = false;          // all of its paths are there
    std::string installed_version;   // from features/installed.txt ("" = found on disk, not installed by us)
    bool selected = false;           // what the player wants (installed or to install)
};

enum FeaturePhase { FP_IDLE, FP_DOWNLOADING, FP_VERIFYING, FP_EXTRACTING, FP_REMOVING, FP_DONE, FP_FAILED, FP_CANCELLED };
struct FeatureProgress {
    FeaturePhase phase = FP_IDLE;
    std::string feature, step, error;    // the feature being worked on, what is happening, why it failed
    uint64_t done = 0, total = 0;        // bytes of all downloads (and unpacking, counted as downloads)
    double speed = 0;                    // bytes per second (downloads)
    int features_done = 0, features_total = 0;
};

// loads features/manifest.json (fetching it first when online); err explains why the list is empty
bool features_load(std::string& err, bool refresh = false);
std::vector<Feature>& features();
std::string features_source();                    // where the list came from ("" = none)
// what installing the selection would take: bytes to download, bytes on disk after, bytes freed
void features_plan(uint64_t& download, uint64_t& add_disk, uint64_t& free_disk);
uint64_t features_disk_free();                    // free bytes on the drive of the game folder
void features_select(const std::string& id, bool on);   // with what it needs (requires) and against what it excludes
bool features_changes_pending();
// installs the selected features that aren't installed and removes the unselected ones, on a thread
void features_apply();
FeatureProgress features_progress();
std::vector<std::pair<std::string, bool>> features_job();   // the running (or last) install's steps: name, install (else remove)
void features_cancel();
bool features_busy();
std::string human_bytes(uint64_t b);
// the preview image of a feature, downloaded into features/previews/ (empty path while it isn't there yet)
std::string features_preview_path(const FeaturePreview& p, bool after);
void features_fetch_previews();                    // in the background
void features_load_async(bool refresh = false);   // features_load on a thread
int features_load_state();                          // 0 not started, 1 loading, 2 loaded, 3 failed
std::string features_load_error();
