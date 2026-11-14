// Steam identity + avatars through the game's own steam_api64.dll (flat API).
// The game has already initialised Steam; we only read from it.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace steam {

bool Available();
uint64_t LocalId();
std::string LocalName();

// Name for any Steam user (falls back to "" until Steam has it).
std::string Name(uint64_t id);

struct Avatar {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;
};
// Medium (64x64) avatar. Returns false while it is still downloading; asks
// Steam to fetch it on first use.
bool GetAvatar(uint64_t id, Avatar& out);

// Pump Steam callbacks (the game itself never does).
void Pump();

}  // namespace steam
