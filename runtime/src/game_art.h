// Art read from the user's disc for the launcher and the pause menu (see game_art.cpp).
#pragma once
#include "ee_context.h"
#include <string>
#include <vector>

struct ArtImage { std::string name; int w = 0, h = 0; std::vector<u32> px; };   // RGBA (memory order R,G,B,A), alpha 0..255

// Uses the game's open disc if there is one, otherwise opens iso_path until game_art_close().
bool game_art_open(const char* iso_path);
void game_art_close();
// One shape from a shape file inside a BIGF archive on the disc, e.g. ("DATA/FRONTEND/LOGOS.BIG", "l010.ssh", "l010").
// shape "" = the first shape in the file.
bool game_art_load(const char* big, const char* file, const char* shape, ArtImage& out);
// Byte offset and size of a file inside the disc image (for streaming, e.g. the menu music).
bool game_art_file(const char* path, u64& offset, u32& size);
