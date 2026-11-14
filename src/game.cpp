#include "game.h"

#include <windows.h>

#include <cctype>
#include <cmath>
#include <cstring>
#include <atomic>
#include <mutex>

#include "hook.h"
#include "log.h"

namespace game {

// ---- addresses (RVA from image base 0x140000000) ---------------------------
// handle_scene_max(SessionInfo const&, SessionMutator&) - runs once per frame
// on the game thread while a scene is active. Found via the RTTI of its local
// boost::any holder type and confirmed at ~1 call/frame with Frida.
constexpr uintptr_t kRvaHandleSceneMax = 0x1a1fb0;
// game::goto_worldplace(globals+0xd20, u8 mode, SessionInfo*, SessionMutator*,
//                       size_t sceneIdx, opt*, opt*, opt*, opt*)
constexpr uintptr_t kRvaGotoWorldplace = 0x2bd830;
// size_t current_scene_idx(SessionInfo+0x258) - honours a queued scene change.
constexpr uintptr_t kRvaCurrentSceneIdx = 0x2d85a0;
// ns_player::Player::GetSkiDirectionXY(Player const*, Vec2* out)
constexpr uintptr_t kRvaGetSkiDirectionXY = 0x3dcdc0;
// GhostFrame* make_ghost_frame(GhostFrame* out, Player const*, float extrapolate)
// (used by Handler_RealtimeGhostframe and by the character renderer)
constexpr uintptr_t kRvaMakeGhostFrame = 0x14a480;
// void draw_character(GhostFrame const*, CharacterModel const*, ctx, u32 lod,
//                     float, float hue, color*, color*, tint*, list*, list*, list*)
// Called by the character render-data builder for the player, ghosts and
// online-multiplayer clients.
constexpr uintptr_t kRvaDrawCharacter = 0x111200;
// float get_character_color_hue(Settings*)  ("character_color_hue")
constexpr uintptr_t kRvaGetHue = 0x2d70d0;
// Character render-data builder (vtable slot of the character renderer):
// void build(this, FrameInfo*, RenderData**)
constexpr uintptr_t kRvaCharRenderBuild = 0x390820;
constexpr uintptr_t kRvaCharRenderVtblSlot = 0x6d9dc8;

// SessionInfo layout
constexpr size_t kSessPlayers = 0x48;    // Player* (array, stride 0x10d0)
constexpr size_t kSessSceneIdx = 0x258;  // size_t current scene index
constexpr size_t kSessGlobals = 0x2d0;   // GMAGlobals** (shared_ptr)
// SessionMutator layout
constexpr size_t kMutGlobals = 0x50;     // GMAGlobals** (shared_ptr)
// GMAGlobals
constexpr size_t kGlobData = 0x40;       // -> data block
constexpr size_t kDataHeadersBegin = 0x1dba8;  // vector<SceneHeader> (0x70 each)
constexpr size_t kDataHeadersEnd = 0x1dbb0;
constexpr size_t kHeaderStride = 0x70;
constexpr size_t kHeaderName = 0x08;     // std::string internal name
constexpr size_t kGlobWorldplaceArg = 0xd20;
constexpr size_t kGlobSettings = 0xd30;         // settings map
constexpr size_t kGlobCharModels = 0x1230;      // vector<CharacterModel> (0x18 each)
constexpr size_t kDataLodSetting = 0x1d668;     // *(GMAGlobals+0x40)+0x1d668
// FrameInfo (2nd arg of the character render builder)
constexpr size_t kFrameSession = 0x20;          // SessionInfo copy used for rendering
constexpr size_t kFrameCtx = 0x28;              // passed through to draw_character
// RenderData: **arg3 + 0xa8 is the list online-multiplayer clients are drawn into
constexpr size_t kRenderRemoteList = 0xa8;
// Ghost frame layout
constexpr size_t kGhostCountOff = 0x410;
constexpr size_t kGhostSize = 0x418;
// ns_player::Player
constexpr size_t kPlayerFeet = 0x310;    // Vec3<double>
constexpr size_t kPlayerHead = 0x340;    // Vec3<double>

static const uint8_t kSigHandleSceneMax[14] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x18,
                                               0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55};
static const uint8_t kSigGotoWorldplace[15] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x4c,
                                               0x89, 0x48, 0x20, 0x48, 0x89, 0x48, 0x08};
static const uint8_t kSigCurrentSceneIdx[16] = {0x48, 0x83, 0xec, 0x18, 0x48, 0x8b, 0x41, 0x10,
                                                0x4c, 0x8b, 0xc1, 0x48, 0x8b, 0x88, 0x78, 0x3c};
static const uint8_t kSigGetSkiDir[15] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x55,
                                          0x48, 0x8d, 0xa8, 0x68, 0xff, 0xff, 0xff};
static const uint8_t kSigMakeGhostFrame[16] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x18, 0x48,
                                               0x89, 0x48, 0x08, 0x55, 0x48, 0x8d, 0x68, 0xa1};
static const uint8_t kSigDrawCharacter[16] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x08, 0x48,
                                              0x89, 0x70, 0x10, 0x48, 0x89, 0x78, 0x20, 0x55};
static const uint8_t kSigGetHue[13] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x50, 0x0f, 0x10, 0x05, 0xc3, 0xb1, 0x3a, 0x00};
static const uint8_t kSigCharRenderBuild[16] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x20, 0x55,
                                                0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56};

using HandleSceneMaxFn = void(__fastcall*)(void* session, void* mutator);
using GotoWorldplaceFn = void(__fastcall*)(void* arg0, uint64_t mode, void* session, void* mutator, uint64_t sceneIdx,
                                           void* o6, void* o7, void* o8, void* o9);
using CurrentSceneIdxFn = uint64_t(__fastcall*)(void* sessScene);
using GetSkiDirFn = float*(__fastcall*)(void* player, float* out);
using MakeGhostFrameFn = void*(__fastcall*)(void* out, void* player, float extrapolate);
using DrawCharacterFn = void(__fastcall*)(void* frame, void* model, void* ctx, uint32_t lod, float a5, float hue,
                                          void* color, void* color2, void* tint, void* list, void* list2,
                                          void* list3);
using GetHueFn = float(__fastcall*)(void* settings);
using CharRenderBuildFn = void(__fastcall*)(void* self, void* frameInfo, void* renderData);

static uintptr_t g_base = 0;
static HandleSceneMaxFn g_origHsm = nullptr;
static GotoWorldplaceFn g_goto = nullptr;
static CurrentSceneIdxFn g_curIdx = nullptr;
static GetSkiDirFn g_skiDir = nullptr;
static MakeGhostFrameFn g_makeGhost = nullptr;
static DrawCharacterFn g_drawChar = nullptr;
static GetHueFn g_getHue = nullptr;
static CharRenderBuildFn g_origCharBuild = nullptr;

static std::mutex g_mu;
static LocalInfo g_local;
static std::vector<std::string> g_sceneNames;
static std::string g_travelTo;  // pending request
static std::vector<RemoteDraw> g_remoteDraws;
static bool g_rendererHooked = false;
static std::atomic<void*> g_globals{nullptr};

// Ghost frame buffers handed to the game's renderer (kept alive across frames).
struct alignas(16) GhostFrameBuf {
    uint8_t bytes[kGhostSize + 8];
};
static std::vector<GhostFrameBuf> g_frameBufs;

double Now() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

template <class T>
static T Rd(const void* p, size_t off) {
    T v;
    memcpy(&v, (const uint8_t*)p + off, sizeof v);
    return v;
}

// MSVC std::string: {union{char buf[16]; char* ptr}; size_t size; size_t cap}
static std::string MsvcString(const uint8_t* s) {
    size_t size = Rd<size_t>(s, 0x10), cap = Rd<size_t>(s, 0x18);
    if (size > 256) return {};
    const char* d = cap > 15 ? Rd<const char*>(s, 0) : (const char*)s;
    return std::string(d, size);
}

static void ReadSceneNames(void* session) {
    uint8_t* globals = Rd<uint8_t*>(Rd<uint8_t*>(session, kSessGlobals), 0);
    uint8_t* data = Rd<uint8_t*>(globals, kGlobData);
    uint8_t* b = Rd<uint8_t*>(data, kDataHeadersBegin);
    uint8_t* e = Rd<uint8_t*>(data, kDataHeadersEnd);
    size_t n = (size_t)(e - b) / kHeaderStride;
    if (n == 0 || n > 512) return;
    std::vector<std::string> names;
    for (size_t i = 0; i < n; ++i) names.push_back(MsvcString(b + i * kHeaderStride + kHeaderName));
    g_sceneNames = names;
    std::string all;
    for (size_t i = 0; i < names.size(); ++i) all += (i ? ", " : "") + names[i];
    Log("Scenes (%zu): %s", n, all.c_str());
}

static void __fastcall HookHandleSceneMax(void* session, void* mutator) {
    g_origHsm(session, mutator);

    // --- sample local player (we are on the game thread; pointers are live) ---
    LocalInfo li;
    li.stamp = Now();
    uint8_t* player = Rd<uint8_t*>(session, kSessPlayers);
    std::lock_guard<std::mutex> lk(g_mu);
    g_globals = Rd<void*>(Rd<uint8_t*>(session, kSessGlobals), 0);
    if (g_sceneNames.empty()) ReadSceneNames(session);
    uint64_t rawIdx = Rd<uint64_t>(session, kSessSceneIdx);
    uint64_t idx = g_curIdx((uint8_t*)session + kSessSceneIdx);
    li.sceneIdx = (int)idx;
    li.loading = rawIdx != idx;
    if (idx < g_sceneNames.size()) li.scene = g_sceneNames[idx];
    if (player) {
        for (int i = 0; i < 3; ++i) {
            li.feet[i] = (float)Rd<double>(player, kPlayerFeet + i * 8);
            li.head[i] = (float)Rd<double>(player, kPlayerHead + i * 8);
        }
        float d[2] = {1, 0};
        g_skiDir(player, d);
        if (std::isfinite(d[0]) && std::isfinite(d[1])) {
            li.dir[0] = d[0];
            li.dir[1] = d[1];
        }
        li.valid = std::isfinite(li.feet[0]) && std::isfinite(li.feet[1]) && std::isfinite(li.feet[2]);

        // The game's own pose snapshot of the player (same data its realtime
        // ghost recorder and online multiplayer use).
        alignas(16) uint8_t gf[kGhostSize + 8] = {};
        g_makeGhost(gf, player, 0.f);
        uint64_t n = Rd<uint64_t>(gf, kGhostCountOff);
        if (n > (uint64_t)kMaxBones) n = 0;
        li.pose.count = (int)n;
        memcpy(li.pose.bones, gf, n * kBoneFloats * sizeof(float));

        uint8_t* globals = Rd<uint8_t*>(Rd<uint8_t*>(session, kSessGlobals), 0);
        float hue = g_getHue(globals + kGlobSettings);
        li.hue = std::isfinite(hue) ? hue : 0.f;
    }
    g_local = li;

    // --- pending travel request ---
    if (!g_travelTo.empty()) {
        std::string target = g_travelTo;
        g_travelTo.clear();
        size_t ti = 0;
        while (ti < g_sceneNames.size() && g_sceneNames[ti] != target) ++ti;
        if (ti == g_sceneNames.size()) {
            Log("Travel: unknown scene '%s'", target.c_str());
        } else if (ti != idx && !li.loading) {
            Log("Travel: %s -> %s (idx %zu)", li.scene.c_str(), target.c_str(), ti);
            // Four empty std::optional-like out/in params, as the game's own
            // "restart scene" command passes them.
            alignas(16) uint8_t o[4][64] = {};
            uint8_t* globals = Rd<uint8_t*>(Rd<uint8_t*>(mutator, kMutGlobals), 0);
            g_goto(globals + kGlobWorldplaceArg, 3, session, mutator, ti, o[0], o[1], o[2], o[3]);
        }
    }
}

// Runs whenever the game builds the render data for all characters (once per
// frame, main thread). After the game has added its own skiers we add the
// remote players exactly the way its online-multiplayer code adds clients.
static void __fastcall HookCharRenderBuild(void* self, void* frameInfo, void* renderData) {
    g_origCharBuild(self, frameInfo, renderData);

    std::lock_guard<std::mutex> lk(g_mu);
    if (g_remoteDraws.empty()) return;
    uint8_t* sess = Rd<uint8_t*>(frameInfo, kFrameSession);
    void* ctx = Rd<void*>(frameInfo, kFrameCtx);
    if (!sess || !renderData) return;
    uint8_t* globals = Rd<uint8_t*>(Rd<uint8_t*>(sess, kSessGlobals), 0);
    uint8_t* models = Rd<uint8_t*>(globals, kGlobCharModels);
    uint8_t* data = Rd<uint8_t*>(globals, kGlobData);
    uint8_t* rd = Rd<uint8_t*>(Rd<uint8_t*>(renderData, 0), 0);
    if (!models || !data || !rd) return;
    uint32_t lod = Rd<uint32_t>(data, kDataLodSetting);
    void* list = rd + kRenderRemoteList;

    if (g_frameBufs.size() < g_remoteDraws.size()) g_frameBufs.resize(std::min<size_t>(g_remoteDraws.size(), 64));
    alignas(16) static uint8_t zeros[3][64] = {};
    for (size_t i = 0; i < g_remoteDraws.size() && i < g_frameBufs.size(); ++i) {
        const RemoteDraw& r = g_remoteDraws[i];
        if (r.pose.count <= 0) continue;
        uint8_t* gf = g_frameBufs[i].bytes;
        memset(gf, 0, sizeof g_frameBufs[i].bytes);
        memcpy(gf, r.pose.bones, r.pose.count * kBoneFloats * sizeof(float));
        uint64_t n = (uint64_t)r.pose.count;
        memcpy(gf + kGhostCountOff, &n, sizeof n);
        // Same arguments the game's own online-multiplayer code uses for a
        // remote client (it passes null for the last two lists, which only the
        // local player's draw fills).
        g_drawChar(gf, models /* character model 0, as for the local player */, ctx, lod, 0.02f, r.hue, zeros[0],
                   zeros[1], zeros[2], list, nullptr, nullptr);
    }
}

bool Init(uintptr_t exeBase) {
    g_base = exeBase;
    auto at = [&](uintptr_t rva) { return (void*)(exeBase + rva); };
    if (memcmp(at(kRvaGotoWorldplace), kSigGotoWorldplace, sizeof kSigGotoWorldplace) != 0 ||
        memcmp(at(kRvaCurrentSceneIdx), kSigCurrentSceneIdx, sizeof kSigCurrentSceneIdx) != 0 ||
        memcmp(at(kRvaGetSkiDirectionXY), kSigGetSkiDir, sizeof kSigGetSkiDir) != 0 ||
        memcmp(at(kRvaMakeGhostFrame), kSigMakeGhostFrame, sizeof kSigMakeGhostFrame) != 0 ||
        memcmp(at(kRvaDrawCharacter), kSigDrawCharacter, sizeof kSigDrawCharacter) != 0 ||
        memcmp(at(kRvaGetHue), kSigGetHue, sizeof kSigGetHue) != 0 ||
        memcmp(at(kRvaCharRenderBuild), kSigCharRenderBuild, sizeof kSigCharRenderBuild) != 0) {
        Log("Unknown game build: signature mismatch. Mod disabled.");
        return false;
    }
    g_goto = (GotoWorldplaceFn)at(kRvaGotoWorldplace);
    g_curIdx = (CurrentSceneIdxFn)at(kRvaCurrentSceneIdx);
    g_skiDir = (GetSkiDirFn)at(kRvaGetSkiDirectionXY);
    g_makeGhost = (MakeGhostFrameFn)at(kRvaMakeGhostFrame);
    g_drawChar = (DrawCharacterFn)at(kRvaDrawCharacter);
    g_getHue = (GetHueFn)at(kRvaGetHue);
    g_frameBufs.reserve(64);  // never reallocates under the renderer's feet

    void** slot = (void**)at(kRvaCharRenderVtblSlot);
    if (*slot == at(kRvaCharRenderBuild)) {
        g_origCharBuild = (CharRenderBuildFn)*slot;
        hook::WritePtr(slot, (void*)&HookCharRenderBuild);
        g_rendererHooked = true;
    } else {
        Log("Character renderer vtable slot unexpected; remote skiers disabled");
    }
    g_origHsm = (HandleSceneMaxFn)hook::Detour(at(kRvaHandleSceneMax), kSigHandleSceneMax, sizeof kSigHandleSceneMax,
                                               (void*)&HookHandleSceneMax);
    if (!g_origHsm) {
        Log("Failed to hook handle_scene_max (signature mismatch). Mod disabled.");
        return false;
    }
    Log("Game hooks installed (base %p)", (void*)exeBase);
    return true;
}

LocalInfo Local() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_local;
}

void SetRemoteDraws(std::vector<RemoteDraw> draws) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_remoteDraws = std::move(draws);
}

bool RendererHooked() { return g_rendererHooked; }
void* Globals() { return g_globals; }

std::vector<std::string> SceneNames() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_sceneNames;
}

void RequestTravel(const std::string& scene) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_travelTo = scene;
}

std::string PrettyScene(const std::string& s) {
    std::string out;
    bool up = true;
    for (char c : s) {
        if (c == '_') {
            out += ' ';
            up = true;
        } else {
            out += up ? (char)toupper((unsigned char)c) : c;
            up = false;
        }
    }
    return out;
}

}  // namespace game
