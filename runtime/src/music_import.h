// Your own music in the game (see music_import.cpp): files are converted once to the game's music format
// (EA "SCHl" stream, EA-XA ADPCM, 32 kHz stereo) and kept in music/. At start they are served to the game in
// place of its nine menu songs; the launcher plays them too.
#pragma once
#include "ee_context.h"
#include <string>
#include <vector>

struct MusicTrack { std::string path, artist, title; double seconds = 0; };

std::vector<MusicTrack> music_tracks();                         // music/*.asf, sorted by name
// Converts one audio file into music/<name>.asf. Returns "" or a message for the player.
std::string music_import(const std::string& src);
int  music_remove_all();                                        // deletes the converted copies in music/
// At game start (disc open, other mods already added). mode 1 = only your music, 2 = mixed with the game's.
// Returns the number of song slots that now play your music.
int  music_apply(int mode);
bool music_is_audio_file(const std::string& path);              // by extension
// tools / tests
bool music_decode_asf(const std::string& path, std::vector<s16>& pcm, int& rate, int& ch);
bool music_write_wav(const std::string& path, const std::vector<s16>& pcm, int rate, int ch);
