// The game's string tables (FEENG.LOC, IGENG.LOC): "LOCH" header, "LOCI" id table, then "LOCL": size, 0, count,
// offsets (from LOCL) and UTF-16 strings. Mods can change text without replacing the whole file: strings.txt in a
// mod folder lists "2005 text<TAB>new text" pairs that are replaced wherever they appear.
#pragma once
#include "ee_context.h"
#include <string>
#include <vector>

struct LocFile {
    std::vector<std::u16string> str;
    bool parse(const std::vector<u8>& d);
    std::vector<u8> build() const;            // the file with the strings as they are now
private:
    std::vector<u8> head_, tail_; u32 zero_ = 0;
};
std::u16string loc_utf16(const std::string& utf8, size_t max_chars = (size_t)-1, bool latin1_only = false);
// Applies dir/strings.txt to both string tables of the open disc. Returns the number of strings changed.
int loc_apply_strings(const char* dir);
// Club names in the menus (FEENG / IGENG.LOC nicknames and full names), by art id 1..30 ("" keeps the disc's): at
// boot, with room to grow; later in the game's memory (era packs' dynasties rename clubs as the seasons go by).
int loc_set_club_names(const std::string* nick, const std::string* full);
int loc_ram_set_club(int art, const std::string& nick, const std::string& full);
int loc_track(const std::string& key, const std::vector<std::string>& values);   // at boot: strings that will change
int loc_ram_set(const std::string& key, const std::string& text);                  // later: in the game's memory
void loc_ram_refresh();                                                            // re-applies after the game reloads them
