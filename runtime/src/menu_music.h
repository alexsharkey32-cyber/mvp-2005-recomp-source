// Menu music while the launcher is open: the game's songs from the user's disc image and/or the player's own
// imported songs (see menu_music.cpp, music_import.cpp).
#pragma once
#include "ee_context.h"
#include <string>
// mode: 0 the disc's songs, 1 your music, 2 both. Disc songs need the disc open through game_art_open().
// volume 0..1. Returns false when there is no audio device or nothing to play.
bool menu_music_start(const char* iso_path, float volume, int mode);
void menu_music_update(float volume);     // every frame: keeps about half a second decoded ahead
void menu_music_stop();
bool menu_music_playing();
std::string menu_music_label();           // what is playing, "" when silent
