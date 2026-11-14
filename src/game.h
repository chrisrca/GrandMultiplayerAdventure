// Game-specific memory access for gma_wonderlands.exe (Steam build,
// SHA-256 766927fdbe20e687d85e99b3ed988eac8cf50f782644363812561f1f73629011).
// All offsets were recovered with REA (Ghidra static analysis) and confirmed
// at runtime with Frida.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace game {

// The game draws every skier (local player, ghosts, online clients) from a
// "ghost frame": up to 26 bones, each {start xyz, end xyz, dir xyz, w} in world
// space. We capture the local player's frame and feed remote players' frames
// back into the game's own character renderer, so they look exactly like the
// real thing (model, outfit hue, animation, lighting, occlusion, post-fx).
constexpr int kMaxBones = 26;
constexpr int kBoneFloats = 10;

struct Pose {
    int count = 0;
    float bones[kMaxBones][kBoneFloats] = {};
};

// Verifies the code bytes at every patch site and installs the hooks.
// Returns false (mod stays inert) on an unknown build.
bool Init(uintptr_t exeBase);

struct LocalInfo {
    bool valid = false;      // a player exists and the game loop is running
    float feet[3] = {0, 0, 0};
    float head[3] = {0, 0, 1};
    float dir[2] = {1, 0};   // ski direction in the XY plane
    Pose pose;               // the game's own ghost frame of the local player
    float hue = 0;           // the player's "character_color_hue" setting
    int sceneIdx = -1;
    std::string scene;       // internal name, e.g. "waldtal"
    bool loading = false;    // a scene change is queued/in progress
    double stamp = 0;        // QPC seconds when sampled
};

LocalInfo Local();

// Remote skiers to draw this frame through the game's renderer.
struct RemoteDraw {
    uint32_t uid;
    float hue;
    Pose pose;
};
void SetRemoteDraws(std::vector<RemoteDraw> draws);
bool RendererHooked();
// GMAGlobals* (null until the first scene frame). Stable for the game's lifetime.
void* Globals();

std::vector<std::string> SceneNames();
void RequestTravel(const std::string& scene);
std::string PrettyScene(const std::string& internalName);
double Now();

}  // namespace game
