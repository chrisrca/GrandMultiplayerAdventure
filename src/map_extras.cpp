#include "map_extras.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>

#include "game.h"
#include "gui.h"
#include "hook.h"
#include "log.h"

namespace map_extras {

// ---- game code (RVAs, see docs/RE_NOTES.md "Piste map: results and travel") ---------
// GameMenuRenderer_MinimapModern vtable slot 2: the map's update
//   (self, SessionInfo const&, ...). Builds the info panel and the action button,
//   handles clicks and runs the button's action.
constexpr uintptr_t kRvaMapUpdate = 0x22a8e0;
constexpr uintptr_t kSlotMapUpdate = 0x6c8080;
// Result rows for a challenge's info panel (small vector of 0x28-byte rows,
// 12 inline), sorted best first and cut to 12:
//   rows* result_rows(rows* out, ..., ChallengeInfo* p7 (+0x80 std::string name),
//                     SceneAchievements* p8, ...)
constexpr uintptr_t kRvaResultRows = 0x224950;
static const uint8_t kSigResultRows[16] = {0x48, 0x89, 0x5c, 0x24, 0x18, 0x55, 0x56, 0x57,
                                           0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
// The map's action button ("Go to challenge", "Place marker here", ...):
//   Button* make_button(Button* out, SessionInfo const&, highlight*, ?, float* rect, float)
constexpr uintptr_t kRvaMakeButton = 0x064340;
static const uint8_t kSigMakeButton[15] = {0x48, 0x8b, 0xc4, 0x55, 0x53, 0x56, 0x57, 0x41,
                                           0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
// vec4* challenge_start(vec4* out, ChallengeInfo*): {x, y, dir_x, dir_y} where a
// "go to challenge" puts the player (10 m before the start gate).
constexpr uintptr_t kRvaStartXf = 0x18b1b0;
static const uint8_t kSigStartXf[17] = {0x4c, 0x8b, 0xdc, 0x53, 0x48, 0x81, 0xec, 0x80, 0x00,
                                        0x00, 0x00, 0x48, 0x8b, 0x02, 0x48, 0x8b, 0xd9};
// game::goto_worldplace(globals+0xd20, u8 mode, SessionInfo const&, SessionMutator&,
//                       size_t sceneIdx, optional<u32> challenge, optional lift,
//                       optional ..., optional ...)
constexpr uintptr_t kRvaGoto = 0x2bd830;
static const uint8_t kSigGoto[15] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x4c,
                                     0x89, 0x48, 0x20, 0x48, 0x89, 0x48, 0x08};
// Called by the "go to" actions right after goto_worldplace with *(mutator+0x20).
constexpr uintptr_t kRvaAfterGoto = 0x2d7230;
static const uint8_t kSigAfterGoto[9] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x0f, 0x10, 0x05};
// void* operator_new(size_t) and ~u32string() (the game's allocator)
constexpr uintptr_t kRvaNew = 0x0022e0;
static const uint8_t kSigNew[16] = {0x48, 0x83, 0xec, 0x28, 0x48, 0x81, 0xf9, 0x00,
                                    0x10, 0x00, 0x00, 0x72, 0x2a, 0x48, 0x8d, 0x41};
constexpr uintptr_t kRvaFreeU32 = 0x0303c0;
static const uint8_t kSigFreeU32[16] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
                                        0x51, 0x18, 0x48, 0x8b, 0xd9, 0x48, 0x83, 0xfa};

// The button's hover/click box is measured from its label (inside the
// builder): measure(scratch, session, builder arg 4, u32string* label,
// float rgb[3], float scale); the box is at scratch+0x440, then free(scratch).
constexpr uintptr_t kRvaMeasureButton = 0x065d60;
static const uint8_t kSigMeasureButton[16] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x18, 0x48,
                                              0x89, 0x70, 0x20, 0x55, 0x57, 0x41, 0x56, 0x48};
constexpr uintptr_t kRvaFreeMeasure = 0x065c20;
static const uint8_t kSigFreeMeasure[16] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x83,
                                            0xb9, 0x10, 0x02, 0x00, 0x00, 0x00, 0x48, 0x8b};
constexpr size_t kMeasureBox = 0x440;

constexpr size_t kSessGlobals = 0x2d0;    // SessionInfo: GMAGlobals** (shared_ptr)
constexpr size_t kMutGlobals = 0x50;      // SessionMutator: GMAGlobals** (shared_ptr)
constexpr size_t kMutAfterGotoArg = 0x20;
constexpr size_t kGlobGotoArg = 0xd20;
// GMAGlobals+0xd68: std::map<std::string, std::string> of saved-game values, e.g.
// "scene_achievements.waldtal.cname_to_results_.<challenge>" = "31.81 32.35"
constexpr size_t kGlobSavedStrings = 0xd68;
constexpr size_t kChallengeName = 0x80;   // ChallengeInfo: std::string
// Button (0xc0): u32 shown, u32string label @+0x08, float box[4] @+0x30
// (hover / click area, from the label), float fade @+0x40 (0..1; the builder
// gets last frame's and steps it +0.1 when there is something to do, else -0.1;
// the button is dropped at <= 0.01),
// std::function<void(SessionInfo const&, SessionMutator&, MinimapState&,
// optional<highlight_t>&)> action @+0x48 (impl pointer @+0x80)
constexpr size_t kButtonLabel = 0x08, kButtonBox = 0x30, kButtonFade = 0x40, kButtonAction = 0x48,
                 kButtonActionImpl = 0x80, kButtonColor = 0x90;
// The go-to buttons' text colour (the builder eases the colour at +0x90 towards
// it; the update keeps it for the next frame).
static const float kGoToColor[4] = {1.f, 0.9f, 0.7f, 1.f};
constexpr size_t kMaxRows = 12;  // what the game keeps (and has room for)
// The map's state: **(update arg 6 + 0x10). The game's selection (what was
// clicked: a challenge, lift, ...; all zero = nothing) and where it draws the
// hover ring ("minimap_icon_highlight_hover_r"): the selection's spot, else the
// point of interest nearest the cursor.
constexpr size_t kStateSelection = 0x140, kSelectionSize = 0x60;
constexpr size_t kStateHoverRing = 0x5e0;  // optional<vec3>: x, y, z, u32 engaged
// The map camera (0x28 bytes each): where it is heading (+0x20), where it is
// (+0x48, eased towards the target) and the one from before a click selected
// something (+0x70; deselecting heads back to it). On a click that selects
// something the game saves +0x48 to +0x70 and aims +0x20 at the selection with
// zoom_to_points_by_fov(out, float viewport[2], camera* current,
// float fov_range[2], vector<vec3>* world points, std::function<Vec3(Vec3
// const&)>* to map space).
constexpr size_t kStateCamTarget = 0x20, kStateCamNow = 0x48, kStateCamBefore = 0x70, kCamSize = 0x28;
constexpr uintptr_t kRvaZoomToPoints = 0x226e10;
static const uint8_t kSigZoomToPoints[14] = {0x48, 0x8b, 0xc4, 0x55, 0x53, 0x56, 0x57,
                                             0x41, 0x54, 0x41, 0x56, 0x41, 0x57, 0x48};
// GMAGlobals+0x1360: the viewport the map uses (0 = 1); "minimap.fov_range" is
// read once into a static.
constexpr size_t kGlobViewport = 0x1360;
constexpr uintptr_t kRvaFovRange = 0x7bee18;
// The map's point-of-interest picker (the hover ring / click target):
// pick(out, candidates, std::function<Vec3(Vec3 const&)>* to map space, view,
// float* cursor, float radius). Called by the map update every frame while
// nothing is selected: its to-map-space function (a lambda holding three
// floats, kept inline: impl pointer at +0x38) is copied for our zooms.
constexpr uintptr_t kRvaPick = 0x0ecc90;
static const uint8_t kSigPick[19] = {0x4c, 0x8b, 0xdc, 0x49, 0x89, 0x5b, 0x08, 0x49, 0x89, 0x6b,
                                     0x10, 0x49, 0x89, 0x73, 0x18, 0x49, 0x89, 0x7b, 0x20};
constexpr size_t kStdFunctionSize = 0x40, kStdFunctionImpl = 0x38;

template <class T>
static T Rd(const void* p, size_t off) {
    T v;
    memcpy(&v, (const uint8_t*)p + off, sizeof v);
    return v;
}

// ---- MSVC strings ---------------------------------------------------------------
struct U32String {
    union {
        char32_t buf[4];
        char32_t* ptr;
    };
    size_t size;
    size_t cap;
};
struct Row {  // the game's result row
    U32String label;
    float value;
    uint8_t kind;   // 0 medal, 3 a saved result, 4 last, 5 personal best, 6/7 new
    uint8_t sport;  // equipment icon
    uint8_t pad[2];
};
static_assert(sizeof(Row) == 0x28, "result row layout");

using NewFn = void*(__fastcall*)(size_t);
using FreeU32Fn = void(__fastcall*)(U32String*);
static NewFn g_new;
static FreeU32Fn g_freeU32;

static std::string StdString(const void* s) {  // MSVC std::string
    size_t n = Rd<size_t>(s, 0x10), cap = Rd<size_t>(s, 0x18);
    if (n > 4096) return "";
    const char* d = cap > 15 ? Rd<const char*>(s, 0) : (const char*)s;
    return std::string(d, n);
}

static std::u32string Utf32(const std::string& s) {
    std::u32string r;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        char32_t cp;
        int n;
        if (c < 0x80) cp = c, n = 1;
        else if ((c >> 5) == 6) cp = c & 0x1f, n = 2;
        else if ((c >> 4) == 14) cp = c & 0x0f, n = 3;
        else cp = c & 0x07, n = 4;
        for (int k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | (s[i + k] & 0x3f);
        r += cp;
        i += n;
    }
    return r;
}

static void MakeU32(U32String* out, const std::string& utf8) {
    std::u32string s = Utf32(utf8);
    memset(out, 0, sizeof *out);
    out->size = s.size();
    if (s.size() <= 3) {
        out->cap = 3;
        memcpy(out->buf, s.c_str(), (s.size() + 1) * 4);
        return;
    }
    out->cap = s.size() | 3;
    out->ptr = (char32_t*)g_new((out->cap + 1) * 4);
    memcpy(out->ptr, s.c_str(), (s.size() + 1) * 4);
}

static std::string Ascii(const U32String* s) {
    const char32_t* d = s->cap > 3 ? s->ptr : s->buf;
    std::string r;
    for (size_t i = 0; i < s->size && i < 200; ++i) r += d[i] < 0x80 ? (char)d[i] : '?';
    return r;
}

// ---- our saved results ------------------------------------------------------------
static std::mutex g_mu;
static mp::Records g_local;  // g_mu
static bool g_localDirty = false;

static std::vector<float> Floats(const std::string& s) {
    std::vector<float> v;
    const char* p = s.c_str();
    while (*p) {
        char* e;
        float f = strtof(p, &e);
        if (e == p) break;
        if (std::isfinite(f)) v.push_back(f);
        p = e;
    }
    return v;
}

// Walks the saved-game string map for
//   scene_achievements.<scene>.cname_to_results_.<challenge>       "31.81 32.35"
//   scene_achievements.<scene>.cname_to_result_sports_.<challenge> "1 2"
static void ReadLocal(void* session) {
    uint8_t* globals = Rd<uint8_t*>(Rd<uint8_t*>(session, kSessGlobals), 0);
    if (!globals) return;
    const uint8_t* m = globals + kGlobSavedStrings;
    const uint8_t* head = Rd<const uint8_t*>(m, 0);
    size_t count = Rd<size_t>(m, 8);
    if (!head || count > 200000) return;
    std::map<std::string, std::vector<float>> results, sports;
    static const std::string kPrefix = "scene_achievements.";
    // in-order walk: node {left, parent, right, color, isnil @+0x19, key @+0x20, value @+0x40}
    std::vector<const uint8_t*> stack;
    const uint8_t* n = Rd<const uint8_t*>(head, 8);
    size_t guard = 0;
    while ((!stack.empty() || !Rd<uint8_t>(n, 0x19)) && guard++ <= count) {
        while (!Rd<uint8_t>(n, 0x19)) {
            stack.push_back(n);
            n = Rd<const uint8_t*>(n, 0);
        }
        n = stack.back();
        stack.pop_back();
        std::string k = StdString(n + 0x20);
        if (k.compare(0, kPrefix.size(), kPrefix) == 0) {
            size_t a = kPrefix.size(), b = k.find('.', a), c = b == std::string::npos ? b : k.find('.', b + 1);
            if (c != std::string::npos) {
                std::string scene = k.substr(a, b - a), field = k.substr(b + 1, c - b - 1), cname = k.substr(c + 1);
                if (field == "cname_to_results_") results[scene + "/" + cname] = Floats(StdString(n + 0x40));
                else if (field == "cname_to_result_sports_") sports[scene + "/" + cname] = Floats(StdString(n + 0x40));
            }
        }
        n = Rd<const uint8_t*>(n, 0x10);
    }
    mp::Records rec;
    for (auto& kv : results) {
        const std::vector<float>& sp = sports[kv.first];
        std::vector<mp::Result>& out = rec[kv.first];
        for (size_t i = 0; i < kv.second.size(); ++i) {
            int s = i < sp.size() ? (int)sp[i] : 1;
            out.push_back({kv.second[i], (uint8_t)(s >= 1 && s <= 4 ? s : 1)});
        }
        if (out.empty()) rec.erase(kv.first);
    }
    std::lock_guard<std::mutex> lk(g_mu);
    if (rec.size() != g_local.size() || !std::equal(rec.begin(), rec.end(), g_local.begin(), [](auto& x, auto& y) {
            if (x.first != y.first || x.second.size() != y.second.size()) return false;
            for (size_t i = 0; i < x.second.size(); ++i)
                if (x.second[i].value != y.second[i].value || x.second[i].sport != y.second[i].sport) return false;
            return true;
        })) {
        size_t total = 0;
        for (auto& kv : rec) total += kv.second.size();
        Log("results: %zu challenges with saved results (%zu results)", rec.size(), total);
        g_local = std::move(rec);
        g_localDirty = true;
    }
}

void OnGameTick(void* session) {
    static ULONGLONG last = 0;
    ULONGLONG now = GetTickCount64();
    if (!session || !g_new || now - last < 3000) return;
    last = now;
    ReadLocal(session);
}

bool TakeLocalRecords(mp::Records& out) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!g_localDirty) return false;
    g_localDirty = false;
    out = g_local;
    return true;
}

// ---- other players' results ---------------------------------------------------------
struct Remote {
    std::string name;
    uint32_t version = 0;
    std::map<uint16_t, mp::Records> chunks;
    mp::Records merged;
};
static std::map<uint32_t, Remote> g_remote;  // g_mu

void OnScoreChunk(const mp::ScoreChunk& c) {
    mp::Records part;
    if (!mp::DecodeRecords(c.payload, part)) return;
    std::lock_guard<std::mutex> lk(g_mu);
    Remote& r = g_remote[c.h.sender];
    if (r.version != c.h.version) {
        r.version = c.h.version;
        r.chunks.clear();
    }
    r.chunks[c.h.index] = std::move(part);
    for (auto it = r.chunks.begin(); it != r.chunks.end();)
        it = it->first >= c.h.total ? r.chunks.erase(it) : std::next(it);
    r.merged.clear();
    for (auto& ch : r.chunks)
        for (auto& kv : ch.second) r.merged[kv.first] = kv.second;
}

void SetPlayers(const std::vector<std::pair<uint32_t, std::string>>& players) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (auto it = g_remote.begin(); it != g_remote.end();) {
        auto p = std::find_if(players.begin(), players.end(), [&](auto& q) { return q.first == it->first; });
        if (p == players.end()) {
            it = g_remote.erase(it);
        } else {
            it->second.name = p->second;
            ++it;
        }
    }
    for (auto& p : players) g_remote[p.first].name = p.second;
}

// ---- the map's update: which mountain it shows ------------------------------------
using MapUpdateFn = void(__fastcall*)(void* self, void* session, void* p3, void* p4, void* p5, void* p6);
static MapUpdateFn g_origMapUpdate;
static void* g_updSession = nullptr;  // during the map update, on its thread
static DWORD g_updThread = 0;

using ZoomToPointsFn = float*(__fastcall*)(float* out, float* viewport, void* camera, float* fovRange,
                                           void* points, void* toMap);
using PickFn = void*(__fastcall*)(void* out, void* candidates, void* toMap, void* view, void* cursor, float radius);
static ZoomToPointsFn g_zoomToPoints;
static PickFn g_origPick;
static uintptr_t g_base;
alignas(16) static uint8_t g_toMap[kStdFunctionSize];  // game thread
static bool g_toMapSet = false;
static uint8_t g_camBefore[kCamSize];  // the camera to go back to after a skier
static bool g_camBeforeSet = false;
static float g_followAt[2];  // where the camera was last aimed at the selected skier
static bool g_following = false;

static void* __fastcall HookPick(void* out, void* candidates, void* toMap, void* view, void* cursor, float radius) {
    if (toMap && GetCurrentThreadId() == g_updThread &&
        Rd<void*>(toMap, kStdFunctionImpl) == toMap) {  // the lambda, held inline
        memcpy(g_toMap, toMap, kStdFunctionSize);
        g_toMapSet = true;
    }
    return g_origPick(out, candidates, toMap, view, cursor, radius);
}

// What the game does when a click selects something, for a skier: keep the
// camera to go back to and aim the camera at them.
static void ZoomToSkier(uint8_t* st, void* session, const float feet[3]) {
    if (!g_zoomToPoints || !g_toMapSet) return;
    uint8_t* globals = Rd<uint8_t*>(Rd<uint8_t*>(session, kSessGlobals), 0);
    if (!globals) return;
    float viewport[2] = {Rd<float>(globals, kGlobViewport), Rd<float>(globals, kGlobViewport + 4)};
    for (float& v : viewport)
        if (v == 0.f) v = 1.f;
    float fov[2];
    memcpy(fov, (void*)(g_base + kRvaFovRange), sizeof fov);
    if (fov[0] == 0.f && fov[1] == 0.f) return;  // not read yet
    float point[3] = {feet[0], feet[1], feet[2]};
    float* points[3] = {point, point + 3, point + 3};  // std::vector<vec3>
    alignas(16) uint8_t toMap[kStdFunctionSize];
    memcpy(toMap, g_toMap, sizeof toMap);
    *(void**)(toMap + kStdFunctionImpl) = toMap;
    alignas(16) float out[12] = {};
    g_zoomToPoints(out, viewport, st + kStateCamNow, fov, points, toMap);
    memcpy(st + kStateCamTarget, out, kCamSize);
    memcpy(st + kStateCamBefore, st + kStateCamNow, kCamSize);
    memcpy(g_camBefore, st + kStateCamNow, kCamSize);
    g_camBeforeSet = true;
    g_followAt[0] = feet[0], g_followAt[1] = feet[1];
    g_following = true;
}

// Keeps the camera on a selected skier as they move. For one point the fit
// above centres it and backs off along the view direction, so its result moves
// with the point: the target moves as far as the skier did (keeping any zoom /
// turn made since).
static void FollowSkier(uint8_t* st, const float feet[3]) {
    if (!g_following) return;
    float dx = feet[0] - g_followAt[0], dy = feet[1] - g_followAt[1];
    g_followAt[0] = feet[0], g_followAt[1] = feet[1];
    float* target = (float*)(st + kStateCamTarget);
    target[0] += dx;
    target[1] += dy;
}

static void __fastcall HookMapUpdate(void* self, void* session, void* p3, void* p4, void* p5, void* p6) {
    uint8_t* st = p6 ? Rd<uint8_t*>(Rd<uint8_t*>(p6, 0x10), 0) : nullptr;
    uint8_t before[kSelectionSize] = {};
    if (st) memcpy(before, st + kStateSelection, kSelectionSize);
    g_updSession = session;
    g_updThread = GetCurrentThreadId();
    g_origMapUpdate(self, session, p3, p4, p5, p6);
    g_updThread = 0;
    if (!st) return;
    // What the game picked: its selection (from a click) and its hover ring.
    const uint8_t* gs = st + kStateSelection;
    gui::GamePick pick{};
    pick.selChanged = memcmp(before, gs, kSelectionSize) != 0;
    for (size_t flag : {0x14, 0x20, 0x30, 0x40, 0x50, 0x58})
        if (gs[flag]) pick.selEngaged = true;
    memcpy(pick.sel, gs, sizeof pick.sel);
    pick.hoverEngaged = (Rd<uint32_t>(st, kStateHoverRing + 12) & 0xff) != 0;
    memcpy(pick.hover, st + kStateHoverRing, sizeof pick.hover);
    gui::NoteGamePick(pick);
    const gui::CameraAction cam = gui::TakeCameraAction();
    // A skier deselected: back to the camera from before, as the game does.
    if (cam == gui::kCamBack && g_camBeforeSet) {
        memcpy(st + kStateCamTarget, g_camBefore, kCamSize);
        memcpy(st + kStateCamBefore, g_camBefore, kCamSize);
        g_camBeforeSet = false;
        if (pick.selChanged) memset(st + kStateSelection, 0, kSelectionSize);  // the click reached the game
    }
    // A skier selected takes the place of the game's own selection, so the
    // action button is about them (Go to <name>). If the game selected
    // something itself meanwhile (a click it got), that stands.
    uint32_t sel = gui::Selected();
    if (sel && cam != gui::kCamZoomToSelected && pick.selChanged && pick.selEngaged) {
        gui::ClearSelection();
        g_camBeforeSet = false;
        sel = 0;
    }
    if (sel) memset(st + kStateSelection, 0, kSelectionSize);
    gui::Player sp;
    const bool onThisMap = sel && gui::GetPlayer(sel, sp) && sp.onMap && sp.scene == game::ViewedScene(session);
    if (!onThisMap) g_following = false;  // deselected, or gone from this map
    if (cam == gui::kCamZoomToSelected && onThisMap)
        ZoomToSkier(st, session, sp.feet);
    else if (onThisMap)
        FollowSkier(st, sp.feet);
    // The game's hover ring goes on the skier under the cursor (when it is
    // theirs), else the selected one.
    uint32_t lit = gui::HoveredPin();
    if (!lit) lit = sel;
    gui::Player p;
    if (lit && gui::GetPlayer(lit, p) && p.onMap && p.scene == game::ViewedScene(session)) {
        float ring[3] = {p.feet[0], p.feet[1], p.feet[2]};
        uint32_t engaged = 1;
        memcpy(st + kStateHoverRing, ring, sizeof ring);
        memcpy(st + kStateHoverRing + 12, &engaged, 4);
    }
}

// ---- result rows ------------------------------------------------------------------
using ResultRowsFn = int64_t*(__fastcall*)(int64_t* out, void*, void*, void*, void*, void*, void* challenge,
                                           void* achievements, void*, void*, void*);
static ResultRowsFn g_origRows;

static void AddOthersRows(int64_t* vec, void* challenge) {
    if (g_updThread != GetCurrentThreadId() || !challenge) return;
    std::string scene = game::ViewedScene(g_updSession);
    std::string key = scene + "/" + StdString((uint8_t*)challenge + kChallengeName);
    struct Extra {
        float value;
        uint8_t sport;
        std::string name;
    };
    std::vector<Extra> extra;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        for (auto& kv : g_remote) {
            auto it = kv.second.merged.find(key);
            if (it == kv.second.merged.end() || kv.second.name.empty()) continue;
            for (const mp::Result& r : it->second) extra.push_back({r.value, r.sport, kv.second.name});
        }
    }
    static std::string lastKey;
    if (key != lastKey) {
        lastKey = key;
        Log("results: '%s' (%zu from other players)", key.c_str(), extra.size());
    }
    if (extra.empty()) return;

    Row* rows = (Row*)vec[0];
    size_t n = (size_t)vec[1], cap = (size_t)vec[2];
    if (!rows || n > 64 || cap < kMaxRows) return;
    // The list is sorted best first: lower is better for times, higher for points.
    bool ascending = n < 2 || rows[0].value <= rows[n - 1].value;
    auto better = [&](float a, float b) { return ascending ? a < b : a > b; };

    // Merge (original rows keep their order; others go after equal values).
    struct Item {
        int orig;  // index into rows, or -1
        size_t extra;
        float value;
    };
    std::vector<Item> all;
    for (size_t i = 0; i < n; ++i) all.push_back({(int)i, 0, rows[i].value});
    for (size_t e = 0; e < extra.size(); ++e) {
        size_t at = 0;
        while (at < all.size() && !better(extra[e].value, all[at].value)) ++at;
        all.insert(all.begin() + at, {-1, e, extra[e].value});
    }
    size_t keep = std::min(all.size(), kMaxRows);
    std::vector<Row> out(keep);
    std::vector<bool> used(n, false);
    for (size_t i = 0; i < keep; ++i) {
        const Item& it = all[i];
        if (it.orig >= 0) {
            memcpy(&out[i], &rows[it.orig], sizeof(Row));  // moved
            used[it.orig] = true;
        } else {
            const Extra& x = extra[it.extra];
            memset(&out[i], 0, sizeof(Row));
            MakeU32(&out[i].label, x.name);
            out[i].value = x.value;
            out[i].kind = 3;  // a saved result: name + equipment icon + value
            out[i].sport = x.sport >= 1 && x.sport <= 4 ? x.sport : 1;
        }
    }
    for (size_t i = 0; i < n; ++i)
        if (!used[i]) g_freeU32(&rows[i].label);  // pushed off the end
    memcpy(rows, out.data(), keep * sizeof(Row));
    vec[1] = (int64_t)keep;
}

static int64_t* __fastcall HookResultRows(int64_t* out, void* a2, void* a3, void* a4, void* a5, void* a6,
                                          void* challenge, void* achievements, void* a9, void* a10, void* a11) {
    int64_t* r = g_origRows(out, a2, a3, a4, a5, a6, challenge, achievements, a9, a10, a11);
    AddOthersRows(out, challenge);
    return r;
}

// ---- "Go to <name>" -----------------------------------------------------------------
using GotoFn = void(__fastcall*)(void* arg0, uint64_t mode, void* session, void* mutator, uint64_t sceneIdx,
                                 void* challenge, void* o7, void* o8, void* o9);
using AfterGotoFn = void(__fastcall*)(void*);
using StartXfFn = float*(__fastcall*)(float* out, void* challenge);
using MakeButtonFn = void*(__fastcall*)(void* out, void* session, void* highlight, void* p4, float* p5, float p6);
static GotoFn g_goto;
static AfterGotoFn g_afterGoto;
static StartXfFn g_origStartXf;
static MakeButtonFn g_origMakeButton;
using MeasureButtonFn = void*(__fastcall*)(void* scratch, void* session, void* p4, void* label, float* rgb,
                                           float scale);
using FreeMeasureFn = void(__fastcall*)(void* scratch);
static MeasureButtonFn g_measureButton;
static FreeMeasureFn g_freeMeasure;

// While travelling to a skier: the spot "go to challenge" uses, on this thread.
static float g_override[4];
static DWORD g_overrideThread = 0;
static bool g_overrideUsed = false;

static float* __fastcall HookStartXf(float* out, void* challenge) {
    if (g_overrideThread && g_overrideThread == GetCurrentThreadId()) {
        memcpy(out, g_override, sizeof g_override);
        g_overrideUsed = true;
        return out;
    }
    return g_origStartXf(out, challenge);
}

static void Travel(uint32_t id, void* session, void* mutator, uint8_t* state) {
    gui::Player p;
    if (!gui::GetPlayer(id, p) || !p.onMap) {
        Log("travel: skier %08X is gone", id);
        return;
    }
    std::vector<std::string> scenes = game::SceneNames();
    size_t idx = std::find(scenes.begin(), scenes.end(), p.scene) - scenes.begin();
    if (idx >= scenes.size()) {
        Log("travel: unknown mountain '%s'", p.scene.c_str());
        return;
    }
    float dx = p.dir[0], dy = p.dir[1], l = sqrtf(dx * dx + dy * dy);
    if (l < 1e-3f) dx = 1, dy = 0;
    else dx /= l, dy /= l;
    // a few metres behind them, facing the way they face
    g_override[0] = p.feet[0] - dx * 6.f;
    g_override[1] = p.feet[1] - dy * 6.f;
    g_override[2] = dx;
    g_override[3] = dy;
    Log("travel: to '%s' on %s at (%.1f, %.1f)", p.name.c_str(), p.scene.c_str(), g_override[0], g_override[1]);
    // The way the map's "Go to challenge" calls it, with the challenge's start
    // spot replaced by ours (HookStartXf).
    alignas(16) uint8_t opt[4][32] = {};
    opt[0][4] = 1;  // optional<u32> challenge = 0 (only its start spot is used)
    uint8_t* globals = Rd<uint8_t*>(Rd<uint8_t*>(mutator, kMutGlobals), 0);
    g_overrideUsed = false;
    g_overrideThread = GetCurrentThreadId();
    g_goto(globals + kGlobGotoArg, 3, session, mutator, idx, opt[0], opt[1], opt[2], opt[3]);
    g_overrideThread = 0;
    if (!g_overrideUsed) Log("travel: the destination was not used");
    g_afterGoto(Rd<void*>(mutator, kMutAfterGotoArg));
    // and clear the map's selection like the game's go-to actions do
    memset(state + 0x130, 0, 0x60);
    memcpy(state + 0x10, state + 0x60, 0x28);
    gui::ClearSelection();
}

// The button's action: a std::function implementation (MSVC _Func_base:
// _Copy, _Move, _Do_call, _Target_type, _Delete_this, _Get).
struct TravelImpl {
    void* const* vtbl;
    uint32_t id;
};
static void* __fastcall TiCopy(TravelImpl* self, void* where) {
    memcpy(where, self, sizeof(TravelImpl));
    return where;
}
static void __fastcall TiCall(TravelImpl* self, void* session, void* mutator, void* state, void*) {
    Travel(self->id, session, mutator, (uint8_t*)state);
}
static const void* __fastcall TiType(TravelImpl*) {
    static const uint64_t kNoType[8] = {};
    return kNoType;
}
static void __fastcall TiDelete(TravelImpl*, uint64_t) {}
static void* __fastcall TiGet(TravelImpl* self) { return (uint8_t*)self + 8; }
static void* const kTravelVtbl[6] = {(void*)&TiCopy, (void*)&TiCopy, (void*)&TiCall,
                                     (void*)&TiType, (void*)&TiDelete, (void*)&TiGet};

static void* __fastcall HookMakeButton(void* out, void* session, void* highlight, void* p4, float* p5, float p6) {
    void* r = g_origMakeButton(out, session, highlight, p4, p5, p6);
    uint32_t sel = gui::Selected();
    if (!sel || !out) return r;
    gui::Player p;
    if (!gui::GetPlayer(sel, p) || !p.onMap) return r;
    uint8_t* b = (uint8_t*)out;
    U32String* label = (U32String*)(b + kButtonLabel);
    if (label->size > 200) return r;
    // On our own mountain with nothing selected the game makes no button (empty
    // label): ours is filled in from scratch.
    const bool fresh = label->size == 0;
    // Mountains we can't go to keep the game's button (buy / unlock).
    std::string was = fresh ? std::string() : Ascii(label);
    if (was.find("urchase") != std::string::npos || was.find("unlock") != std::string::npos ||
        was.find("rocessing") != std::string::npos || was.find("Cannot") != std::string::npos)
        return r;
    if (fresh) memcpy(b + kButtonColor, kGoToColor, sizeof kGoToColor);
    g_freeU32(label);
    MakeU32(label, "Go to " + p.name);
    // With the game's own selection cleared, on our own mountain the game has
    // nothing to do and fades the button out; ours has something to do, so it
    // fades in like the game's go-to buttons.
    float fade = p6 + 0.1f;
    *(float*)(b + kButtonFade) = fade < 0.f ? 0.f : fade > 1.f ? 1.f : fade;
    // The hover / click box was measured from the game's label: measure ours.
    if (g_measureButton) {
        alignas(16) uint8_t scratch[0x460];  // the game's is 0x450
        float rgb[3] = {1.f, 1.f, 1.f};
        g_measureButton(scratch, session, p4, label, rgb, 1.f);
        memcpy(b + kButtonBox, scratch + kMeasureBox, 16);
        g_freeMeasure(scratch);
    }
    void** implSlot = (void**)(b + kButtonActionImpl);
    if (void* impl = *implSlot) {  // destroy the game's action
        void* const* vt = *(void* const**)impl;
        ((void(__fastcall*)(void*, uint64_t))vt[4])(impl, impl != (void*)(b + kButtonAction));
    }
    TravelImpl* ti = (TravelImpl*)(b + kButtonAction);
    ti->vtbl = kTravelVtbl;
    ti->id = sel;
    *implSlot = ti;
    return r;
}

bool Init(uintptr_t base) {
    auto at = [&](uintptr_t rva) { return (void*)(base + rva); };
    if (memcmp(at(kRvaGoto), kSigGoto, sizeof kSigGoto) || memcmp(at(kRvaAfterGoto), kSigAfterGoto, sizeof kSigAfterGoto) ||
        memcmp(at(kRvaNew), kSigNew, sizeof kSigNew) || memcmp(at(kRvaFreeU32), kSigFreeU32, sizeof kSigFreeU32)) {
        Log("map extras: signature mismatch, results / travel disabled");
        return false;
    }
    void** slot = (void**)at(kSlotMapUpdate);
    if (*slot != at(kRvaMapUpdate)) {
        Log("map extras: map update slot unexpected, results / travel disabled");
        return false;
    }
    g_goto = (GotoFn)at(kRvaGoto);
    g_afterGoto = (AfterGotoFn)at(kRvaAfterGoto);
    g_new = (NewFn)at(kRvaNew);
    g_freeU32 = (FreeU32Fn)at(kRvaFreeU32);
    g_base = base;
    if (!memcmp(at(kRvaZoomToPoints), kSigZoomToPoints, sizeof kSigZoomToPoints)) {
        g_zoomToPoints = (ZoomToPointsFn)at(kRvaZoomToPoints);
        g_origPick = (PickFn)hook::Detour(at(kRvaPick), kSigPick, sizeof kSigPick, (void*)&HookPick);
        if (!g_origPick) g_zoomToPoints = nullptr;
    }
    if (!memcmp(at(kRvaMeasureButton), kSigMeasureButton, sizeof kSigMeasureButton) &&
        !memcmp(at(kRvaFreeMeasure), kSigFreeMeasure, sizeof kSigFreeMeasure)) {
        g_measureButton = (MeasureButtonFn)at(kRvaMeasureButton);
        g_freeMeasure = (FreeMeasureFn)at(kRvaFreeMeasure);
    }
    g_origStartXf = (StartXfFn)hook::Detour(at(kRvaStartXf), kSigStartXf, sizeof kSigStartXf, (void*)&HookStartXf);
    g_origRows = (ResultRowsFn)hook::Detour(at(kRvaResultRows), kSigResultRows, sizeof kSigResultRows,
                                            (void*)&HookResultRows);
    if (g_origStartXf)
        g_origMakeButton = (MakeButtonFn)hook::Detour(at(kRvaMakeButton), kSigMakeButton, sizeof kSigMakeButton,
                                                      (void*)&HookMakeButton);
    g_origMapUpdate = (MapUpdateFn)*slot;
    hook::WritePtr(slot, (void*)&HookMapUpdate);
    Log("map extras: results %s, travel %s, camera %s", g_origRows ? "on" : "off", g_origMakeButton ? "on" : "off",
        g_zoomToPoints ? "on" : "off");
    return true;
}

}  // namespace map_extras
