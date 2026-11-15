// Piste-map features beyond the pins (gui.cpp draws those):
//  - Challenge results: everyone's saved results are merged into the result
//    list the map shows for a challenge / trial (the game's own list, at most
//    12 rows), each with the player's name and equipment icon.
//  - "Go to <name>": with another skier selected on the map (gui::MapClick),
//    the map's action button travels to them, on any mountain, through the
//    game's own "go to challenge" travel.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "net.h"

namespace map_extras {

// Verifies the code and installs the hooks. Returns false (features off) on
// an unknown build.
bool Init(uintptr_t exeBase);

// From the game thread once per frame (handle_scene_max, after the game):
// re-reads our own saved results every few seconds.
void OnGameTick(void* session);

// Our results, if they changed since the last call (frame thread).
bool TakeLocalRecords(mp::Records& out);

// Results from other players (frame thread).
void OnScoreChunk(const mp::ScoreChunk& c);
// Players currently connected (id, display name); others' results are dropped.
void SetPlayers(const std::vector<std::pair<uint32_t, std::string>>& players);

}  // namespace map_extras
