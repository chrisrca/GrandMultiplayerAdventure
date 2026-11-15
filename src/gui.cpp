#include "gui.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#include "game.h"
#include "hook.h"
#include "log.h"

namespace gui {

// ---- game GUI API (RVAs, verified with REA + Frida) ---------------------------
// void* gui_text(GuiList*, u32string_view* text, string_view* font, u64 pos(x,y),
//                float size, float* rgba, u8 alignH, u8 alignV, u8 layer, u8 style,
//                u64* shadow)
constexpr uintptr_t kRvaText = 0x1fb800;
// void gui_sprite(GuiList** holder, string_view* texture, float* rect(l,t,r,b),
//                 float* rgba, u8 layer, u8 flag, float angle, float* uv(l,t,r,b))
constexpr uintptr_t kRvaSprite = 0x1fc1e0;
// vec3* map_transform(MapXf*, vec3* out, vec3* world)   (world -> map space)
constexpr uintptr_t kRvaMapXf = 0x0ecfb0;
// vec2* map_project(MapProj*, vec2* out, vec3* map)     (map -> normalized screen)
constexpr uintptr_t kRvaMapProj = 0x50b060;
// GUI passes we append to (vtable slot 3 = "draw GUI" of each class)
constexpr uintptr_t kRvaMetricTextsGui = 0x1d0840;  // Handler_MetricTexts
constexpr uintptr_t kSlotMetricTextsGui = 0x6c34e0;
constexpr uintptr_t kRvaMinimapGui = 0x22f5f0;      // GameMenuRenderer_MinimapModern
constexpr uintptr_t kSlotMinimapGui = 0x6c8088;

static const uint8_t kSigText[15] = {0x48, 0x8b, 0xc4, 0x55, 0x53, 0x56, 0x57, 0x41,
                                     0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
static const uint8_t kSigSprite[10] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x18, 0x55, 0x56, 0x57};
static const uint8_t kSigMapXf[16] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
                                      0x49, 0x38, 0x48, 0x8b, 0xda, 0x48, 0x85, 0xc9};
static const uint8_t kSigMapProj[16] = {0x48, 0x83, 0xec, 0x28, 0x0f, 0x29, 0x74, 0x24,
                                        0x10, 0x33, 0xc0, 0x48, 0x89, 0x02, 0xf3, 0x41};

struct StrView {
    const char* p;
    size_t n;
};
struct U32View {
    const char32_t* p;
    size_t n;
};

using TextFn = void*(__fastcall*)(void* list, const U32View* text, const StrView* font, uint64_t pos, float size,
                                  const float* rgba, uint64_t alignH, uint64_t alignV, uint64_t layer, uint64_t style,
                                  const uint64_t* shadow);
using SpriteFn = void(__fastcall*)(void* holder, const StrView* tex, const float* rect, const float* rgba,
                                   uint64_t layer, uint64_t flag, float angle, const float* uv);
using MapXfFn = float*(__fastcall*)(void* xf, float* out, const float* in);
using MapProjFn = float*(__fastcall*)(void* proj, float* out, const float* in);
using GuiPassFn = void(__fastcall*)(void* self, void* frameInfo, void* holder);
// bool menu_on_top(MenuStack* frameInfo+0x238)  (used by the game's HUD passes)
constexpr uintptr_t kRvaMenuOnTop = 0x26ff40;
static const uint8_t kSigMenuOnTop[16] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x30, 0x4c, 0x8b,
                                          0x09, 0xb8, 0x0b, 0x00, 0x00, 0x00, 0x66, 0x89};
using MenuOnTopFn = uint32_t(__fastcall*)(void* stack);
static MenuOnTopFn g_menuOnTop;
using MinimapPassFn = void(__fastcall*)(void* self, void* frameInfo, void* holder, void* p4, void* p5);
// The piste-map pass also runs while other pages of the menu (Settings,
// Controls, ...) are showing; it checks these first and draws nothing unless
// the map page is up. Each takes frameInfo+0x258 (the frame's SessionInfo is
// frameInfo itself; +0x258 is its scene state).
//   map_page_ready:   menu state == 12              (FUN_1402d8910)
//   map_page_shown:   the map widget is the page    (FUN_1402d87f0)
// The mountain on show is current_scene_idx() (FUN_1402d8840), which follows
// the mountain picked on the map: see game::ViewedScene.
constexpr size_t kFrameMapScene = 0x258;
constexpr uintptr_t kRvaMapPageReady = 0x2d8910, kRvaMapPageShown = 0x2d87f0;
static const uint8_t kSigMapPageReady[11] = {0x48, 0x83, 0xec, 0x28, 0xe8, 0x77, 0xfd, 0xff, 0xff, 0x3c, 0x0c};
static const uint8_t kSigMapPageShown[16] = {0x48, 0x83, 0xec, 0x38, 0x48, 0x8b, 0x49, 0x10,
                                             0x48, 0x8d, 0x44, 0x24, 0x40, 0x48, 0x89, 0x44};
using MapCheckFn = bool(__fastcall*)(void* sceneState);
static MapCheckFn g_mapPageReady, g_mapPageShown;

// ---- textures (how the title screen shows the Steam profile picture) --------
// void gui_image(GuiList** holder, string_view* texture, float* rect(l,t,r,b),
//                float alpha, u8 layer, u8 flag, u64 0, float* uv)   (untinted)
constexpr uintptr_t kRvaImage = 0x1fbf90;
static const uint8_t kSigImage[16] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x48,
                                      0x89, 0x70, 0x18, 0x57, 0x48, 0x81, 0xec, 0xd0};
// gl::GLTextureHolder::upload_impl(holder, GLBigState*, levels, 2, int wh[2],
//     type, pixels, format, internalFormat, target | autoMip << 32, string_view* debugName)
constexpr uintptr_t kRvaTexUpload = 0x520db0;
static const uint8_t kSigTexUpload[16] = {0x48, 0x89, 0x5c, 0x24, 0x20, 0x55, 0x56, 0x57,
                                          0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
// named-texture map: insert_or_replace(map = GMAGlobals+0x48, string_view* name, holder&&)
constexpr uintptr_t kRvaTexInsert = 0x505d20;
static const uint8_t kSigTexInsert[16] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56, 0x48,
                                          0x8d, 0x6c, 0x24, 0xc9, 0x48, 0x81, 0xec, 0xb0};
constexpr size_t kGlobTextureMap = 0x48;
// Global service registry; slot +0x680 holds std::shared_ptr<gl::GLBigState>
// (type_info pointer at +0x6b8, low 2 bits are flags). Ready when +0x1b8 != 0.
constexpr uintptr_t kRvaServices = 0x4f1a40;
static const uint8_t kSigServices[24] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x48, 0x89, 0x6c, 0x24, 0x10, 0x48, 0x89,
                                         0x74, 0x24, 0x18, 0x57, 0x48, 0x83, 0xec, 0x20, 0xba, 0x08, 0x00, 0x00};
constexpr uintptr_t kRvaTexCounter = 0x78c0d0;  // texture holder serial counter
static const char kGLStateType[] = ".?AV?$shared_ptr@VGLBigState@gl@@@std@@";

using ImageFn = void(__fastcall*)(void* holder, const StrView* tex, const float* rect, float alpha, uint64_t layer,
                                  uint64_t flag, uint64_t zero, const float* uv);
using TexUploadFn = void(__fastcall*)(void* holder, void* gl, int64_t levels, uint64_t two, const int* wh,
                                      uint64_t type, const void* pixels, uint64_t format, uint64_t internalFormat,
                                      uint64_t target, const StrView* name);
using TexInsertFn = void(__fastcall*)(void* map, const StrView* name, void* holder);
using ServicesFn = uint8_t*(__fastcall*)();
static ImageFn g_image;
static TexUploadFn g_texUpload;
static TexInsertFn g_texInsert;
static ServicesFn g_services;
static volatile LONG* g_texCounter;
static DWORD g_guiThread = 0;

static TextFn g_text;
static SpriteFn g_sprite;
static MapXfFn g_mapXf;
static MapProjFn g_mapProj;
static GuiPassFn g_origIngame;
static MinimapPassFn g_origMinimap;

static std::mutex g_mu;
static Frame g_frame;

// Alignment codes as used by the game ("Next Mountain" = left, HUD labels = right,
// piste-map labels = centred over their point, bottom aligned).
// (Verified on screen: the 10th argument is the horizontal alignment.)
constexpr uint64_t kAlignCenter = 2, kAlignLeft = 3;  // (0 also left-aligns; right is not used)
// Vertical: 1 = middle, 3 = top (the text hangs below y; measured from the
// text bounds the call returns).
constexpr uint64_t kVAlignMiddle = 1, kVAlignTop = 3;
constexpr float kTextHeight = 0.80f;  // laid-out glyph box height / size (21 px at size 0.025, 1080p)

static std::u32string U32(const std::string& s) {
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

static uint64_t Pack(float x, float y) {
    uint32_t a, b;
    memcpy(&a, &x, 4);
    memcpy(&b, &y, 4);
    return (uint64_t)a | ((uint64_t)b << 32);
}

static void* Text(void* holder, const std::string& s, const char* font, float x, float y, float size, const float rgb[3],
                  float alpha, uint64_t alignH, uint64_t alignV, uint64_t style, bool shadow) {
    if (s.empty() || alpha <= 0.f) return nullptr;
    std::u32string t = U32(s);
    U32View tv{t.data(), t.size()};
    StrView fv{font, strlen(font)};
    const float rgba[4] = {rgb[0], rgb[1], rgb[2], alpha};
    const uint64_t sh = shadow ? 0x100000001ULL : 0;
    // argument order as the game uses it: (style, vertical align, layer, horizontal align)
    return g_text(*(void**)holder, &tv, &fv, Pack(x, y), size, rgba, style, alignV, 0, alignH, &sh);
}

static void Sprite(void* holder, const char* tex, float l, float t, float r, float b, const float rgba[4],
                   uint64_t layer, uint64_t flag, float angle) {
    StrView tv{tex, strlen(tex)};
    const float rect[4] = {l, t, r, b};
    static const float uv[4] = {0, 0, 1, 1};
    g_sprite(holder, &tv, rect, rgba, layer, flag, angle, uv);
}

// The piste map's player marker: "playerdir_r" (ring + pointer, rotated) with
// "playerpos_r" (skier avatar) inside, each with the map's soft drop shadow.
// Sizes are the map's own (captured at runtime), as fractions of screen height.
static void Pin(void* holder, float cx, float cy, float scale, float angle, const float rgb[3], float alpha,
                float aspect) {
    const float hDir = 0.0960f * scale, hPos = 0.0587f * scale;
    const float wDir = hDir / aspect, wPos = hPos / aspect;
    const float shadow[4] = {0, 0, 0, 0.2f * alpha};
    const float col[4] = {rgb[0], rgb[1], rgb[2], alpha};
    const float s = 1.25f;
    Sprite(holder, "playerdir_r", cx - wDir * s / 2, cy - hDir * s / 2, cx + wDir * s / 2, cy + hDir * s / 2, shadow, 1, 0,
           angle);
    Sprite(holder, "playerpos_r", cx - wPos * s / 2, cy - hPos * s / 2, cx + wPos * s / 2, cy + hPos * s / 2, shadow, 1, 0,
           0);
    Sprite(holder, "playerdir_r", cx - wDir / 2, cy - hDir / 2, cx + wDir / 2, cy + hDir / 2, col, 3, 3, angle);
    Sprite(holder, "playerpos_r", cx - wPos / 2, cy - hPos / 2, cx + wPos / 2, cy + hPos / 2, col, 3, 3, 0);
}

static bool Project(const float vp[16], const float p[3], float& x, float& y) {
    float c[4];
    for (int r = 0; r < 4; ++r) c[r] = vp[r] * p[0] + vp[4 + r] * p[1] + vp[8 + r] * p[2] + vp[12 + r];
    float w = c[3];
    bool front = w > 0.05f;
    // Behind the camera w is negative; dividing by it would mirror the point to
    // the opposite side of the screen (the edge pin then points away from the
    // player). Dividing by |w| keeps left as left and below as below.
    float aw = std::max(fabsf(w), 0.05f);
    x = 0.5f + 0.5f * c[0] / aw;
    y = 0.5f - 0.5f * c[1] / aw;
    return front;
}

static std::string Meters(float d) {
    char b[32];
    if (d < 1000) snprintf(b, sizeof b, "%dm", (int)(d + 0.5f));
    else snprintf(b, sizeof b, "%.1fkm", d / 1000.f);
    return b;
}

static float g_aspect = 16.f / 9.f;

// Normalised bounds (l, t, r, b) of text laid out by the text call, from the
// object it returns: pixel bounds at +0x400, screen size at +0x420 (the title
// screen uses these to put the version number after your name).
static bool TextBounds(void* r, float out[4]) {
    if (!r) return false;
    const float* px = (const float*)((uint8_t*)r + 0x400);
    const float* scr = (const float*)((uint8_t*)r + 0x420);
    if (!(scr[0] > 0) || !(scr[1] > 0)) return false;
    out[0] = px[0] / scr[0], out[1] = px[1] / scr[1], out[2] = px[2] / scr[0], out[3] = px[3] / scr[1];
    return out[3] > out[1];
}

static bool Ascii(const std::string& s) {
    for (unsigned char c : s)
        if (c >= 0x80) return false;
    return true;
}
// The game's display fonts only have Latin glyphs; Steam names are shown in
// "mplus1" (as on the title screen), which also covers e.g. Japanese.
static const char* LabelFont(const std::string& s) { return Ascii(s) ? "staatliches" : "mplus1"; }

// At most `n` code points, with "..." when cut.
static std::string Clip(const std::string& s, size_t n) {
    size_t cps = 0, i = 0;
    while (i < s.size()) {
        if (cps == n) return s.substr(0, i) + "...";
        unsigned char c = s[i];
        i += c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : 4;
        ++cps;
    }
    return s;
}

// A Steam avatar the way the title screen draws it: the picture (untinted) over
// a soft white "glyph_gradient_circle_r" glow 1.5x its size. Without an avatar
// the piste map's skier icon stands in, tinted in the player's colour.
static void Avatar(void* holder, const std::string& tex, float cx, float cy, float h, float alpha, const float rgb[3]) {
    const float w = h / g_aspect;
    static const float uv[4] = {0, 0, 1, 1};
    if (tex.empty()) {
        const float col[4] = {rgb[0], rgb[1], rgb[2], alpha};
        Sprite(holder, "playerpos_r", cx - w * 0.6f, cy - h * 0.6f, cx + w * 0.6f, cy + h * 0.6f, col, 3, 3, 0);
        return;
    }
    const float glow[4] = {1, 1, 1, alpha * 0.5f};
    const float g[4] = {cx - w * 0.75f, cy - h * 0.75f, cx + w * 0.75f, cy + h * 0.75f};
    StrView gt{"glyph_gradient_circle_r", 23};
    g_sprite(holder, &gt, g, glow, 2, 1, 0.f, uv);
    const float r[4] = {cx - w * 0.5f, cy - h * 0.5f, cx + w * 0.5f, cy + h * 0.5f};
    StrView tv{tex.data(), tex.size()};
    g_image(holder, &tv, r, alpha, 3, 1, 0, uv);
}

// ---- in-game pass -------------------------------------------------------------
static void DrawIngame(void* holder, const Frame& f) {
    const float aspect = g_aspect;
    static const float kWhite[3] = {0.9f, 0.9f, 0.85f}, kGrey[3] = {0.72f, 0.72f, 0.70f},
                       kOrange[3] = {1.0f, 0.55f, 0.35f};

    // Name labels / edge pins for other skiers on our mountain.
    if (f.viewProjValid) {
        for (const Player& p : f.players) {
            if (p.you || !p.visible) continue;
            float x, y;
            bool front = Project(f.viewProj, p.anchor, x, y);
            const float m = 0.02f;
            bool onScreen = front && x > m && x < 1 - m && y > m && y < 1 - m;
            if (onScreen) {
                if (f.showNames) {
                    // Above the skier's whole on-screen outline, so it never covers
                    // them, whatever the zoom level or camera angle.
                    float top = y;
                    for (size_t i = 0; i + 2 < p.outline.size(); i += 3) {
                        float px, py;
                        if (Project(f.viewProj, &p.outline[i], px, py)) top = std::min(top, py);
                    }
                    float a = std::max(0.5f, std::min(0.95f, 1.15f - p.dist / 900.f));
                    Text(holder, p.name, LabelFont(p.name), x, top - 0.006f - 0.025f * kTextHeight, 0.025f, p.color,
                         a, kAlignCenter, kVAlignTop, 2, false);
                }
                continue;
            }
            // Off-screen: the map's player pin at the screen edge, pointing at them.
            float dx = (x - 0.5f) * aspect, dy = y - 0.5f;
            if (fabsf(dx) < 1e-4f && fabsf(dy) < 1e-4f) dy = 1;
            float scale = std::max(0.55f, std::min(1.0f, 1.1f - p.dist / 800.f));
            float hy = 0.0960f * scale * 0.5f + 0.015f, hx = hy;  // half extents (height units)
            float kx = (aspect * 0.5f - hx) / std::max(fabsf(dx), 1e-4f);
            float ky = (0.5f - hy) / std::max(fabsf(dy), 1e-4f);
            float k = std::min(kx, ky);
            float cx = 0.5f + dx * k / aspect, cy = 0.5f + dy * k;
            float angle = atan2f(dx, -dy);  // clockwise from "up"
            Pin(holder, cx, cy, scale, angle, p.color, 0.95f, aspect);
            if (f.showNames) {
                // label toward the screen centre
                float lx = cx - dx / sqrtf(dx * dx + dy * dy) * hx / aspect * 1.6f;
                float ly = cy - dy / sqrtf(dx * dx + dy * dy) * hy * 1.6f;
                Text(holder, p.name + "  " + Meters(p.dist), LabelFont(p.name), lx, ly, 0.02f * std::max(0.85f, scale),
                     p.color, 0.9f, kAlignCenter, kVAlignMiddle, 2, false);
            }
        }
    }

    // Player list, laid out like the game's Controls screen, middle-left of the
    // screen (clear of the HUD top-left/right and the pop-ups at the top).
    // Each row: Steam avatar (title-screen style), Steam name, status.
    // The toggle key hides it completely. Columns are left-aligned (the game's text
    // call has no working right alignment).
    if (f.showList) {
        const float x0 = 0.022f, avH = 0.030f, xName = x0 + avH / aspect + 0.010f, xStatus = 0.175f;
        // The status line only appears when something needs attention
        // ("Connecting to ...", "Port 47800 is busy"), not for the normal state.
        const bool showSub = !f.subGood && !f.subline.empty();
        const float headH = 0.034f, subH = showSub ? 0.028f : 0.006f, rowH = 0.042f;
        const size_t rows = f.players.size();
        const float total = headH + subH + rows * rowH + 0.024f;  // + hint line
        float y = 0.5f - total * 0.5f + headH * 0.5f;
        const float band[4] = {0, 0, 0, 0.55f};
        static const float uv[4] = {0, 0, 1, 1};
        StrView tex{"glyph_gradient_square_r", 23};
        const float bandRect[4] = {0.0f, y - headH * 0.5f - 0.008f, 0.245f, y - headH * 0.5f + total + 0.008f};
        g_sprite(holder, &tex, bandRect, band, 1, 0, 0.f, uv);

        Text(holder, f.header, "contrailone", x0, y, 0.036f, kWhite, 1.f, kAlignLeft, kVAlignMiddle, 3, true);
        y += headH;
        if (showSub)
            Text(holder, f.subline, "staatliches", x0, y, 0.022f, kOrange, 0.95f, kAlignLeft, kVAlignMiddle, 2, false);
        y += subH;
        {
            for (const Player& p : f.players) {
                float cy = y + rowH * 0.5f - 0.006f;
                float a = p.dim ? 0.5f : 1.f;
                void* r = Text(holder, Clip(p.name, 16), "mplus1", xName, cy, 0.024f, p.color, a, kAlignLeft,
                               kVAlignMiddle, 3, true);
                float box[4];
                Avatar(holder, p.avatar, x0 + avH / aspect * 0.5f, TextBounds(r, box) ? (box[1] + box[3]) * 0.5f : cy,
                       avH, a, p.color);
                if (p.you || p.host)
                    Text(holder, p.you ? "(You)" : "(Host)", "staatliches", xStatus, cy, 0.022f, kOrange, a,
                         kAlignLeft, kVAlignMiddle, 2, false);
                else
                    Text(holder, p.status, "staatliches", xStatus, cy, 0.022f, kGrey, a, kAlignLeft, kVAlignMiddle,
                         2, false);
                y += rowH;
            }
        }
        Text(holder, f.toggleKey + " to hide", "staatliches", x0, y + 0.004f, 0.018f, kGrey, 0.6f, kAlignLeft, kVAlignMiddle, 2, false);
    }

    // Notifications, centred near the top like the game's own headlines, with
    // the player's Steam avatar left of the text. Over a menu they go just above
    // the menu's bottom bar instead, stacking upward.
    const float step = f.toastsAtBottom ? -0.045f : 0.045f;
    float ty = f.toastsAtBottom ? 0.875f : 0.11f;
    for (const Toast& t : f.toasts) {
        void* r = Text(holder, t.text, Ascii(t.text) ? "contrailone" : "mplus1", 0.5f, ty, 0.034f, kWhite, t.alpha,
                       kAlignCenter, kVAlignMiddle, 3, true);
        // The text call returns the laid-out text; its pixel bounds start at
        // +0x400 and the screen size is at +0x420 (the title screen uses them to
        // put the version number right after your name).
        float box[4];
        if (!t.avatar.empty() && TextBounds(r, box) && box[0] > 0.05f && box[0] < 0.5f) {
            // centred on the text as laid out (the "middle" alignment is not the
            // visual centre of the glyphs)
            const float h = 0.036f;
            Avatar(holder, t.avatar, box[0] - 0.012f - h / aspect * 0.5f, (box[1] + box[3]) * 0.5f, h, t.alpha, kWhite);
        }
        ty += step;
    }
}

static ULONGLONG g_lastMapTick = 0;

// ---- picking skiers on the piste map --------------------------------------------
// Pins drawn in the last map frame, for clicks (normalised screen coordinates).
struct PinHit {
    uint32_t id;
    float x, y;
};
static std::vector<PinHit> g_pins;  // g_mu
static uint32_t g_selected = 0;     // g_mu
static HWND g_hwnd = nullptr;
constexpr float kPinPickRadius = 0.045f;  // in screen heights (about the pin's ring)
constexpr float kMapLeft = 0.255f;        // the info panel covers the map left of this

void SetWindow(void* hwnd) { g_hwnd = (HWND)hwnd; }

static bool MapShowing() { return GetTickCount64() - g_lastMapTick < 250; }

static GamePick g_lastPick{};                // g_mu: latest map update
static uint32_t g_hovered = 0;               // g_mu: set by the map frame
static CameraAction g_camAction = kCamNone;  // g_mu

bool MapClick(float x, float y) {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!MapShowing()) return false;
    if (g_selected) {
        // like the game with something selected: a click deselects (except on
        // the action button along the bottom) and the camera goes back
        if (y >= 0.86f) return false;
        Log("map: deselected skier %08X", g_selected);
        g_selected = 0;
        g_camAction = kCamBack;
        return true;
    }
    if (g_lastPick.selEngaged || !g_hovered) return false;  // the game's click
    // like the game with nothing selected: the click picks what has the ring
    g_selected = g_hovered;
    Log("map: selected skier %08X", g_selected);
    g_camAction = kCamZoomToSelected;
    return true;
}

void NoteGamePick(const GamePick& g) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_lastPick = g;
}

CameraAction TakeCameraAction() {
    std::lock_guard<std::mutex> lk(g_mu);
    CameraAction a = g_camAction;
    g_camAction = kCamNone;
    return a;
}

static bool CursorPos(float& x, float& y);

uint32_t HoveredPin() {
    std::lock_guard<std::mutex> lk(g_mu);
    return MapShowing() ? g_hovered : 0;
}

// The nearest pin in reach of (x, y) (0 = none) and its distance (screen heights).
static uint32_t NearestPin(float x, float y, float& dist) {
    uint32_t best = 0;
    float bestD = kPinPickRadius * kPinPickRadius;
    for (const PinHit& p : g_pins) {
        float dx = (x - p.x) * g_aspect, dy = y - p.y, d = dx * dx + dy * dy;
        if (d < bestD) bestD = d, best = p.id;
    }
    dist = sqrtf(bestD);
    return best;
}

uint32_t Selected() {
    std::lock_guard<std::mutex> lk(g_mu);
    if (!MapShowing()) g_selected = 0;
    return g_selected;
}

void ClearSelection() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_selected = 0;
}

bool GetPlayer(uint32_t id, Player& out) {
    std::lock_guard<std::mutex> lk(g_mu);
    for (const Player& p : g_frame.players)
        if (!p.you && p.id == id) {
            out = p;
            return true;
        }
    return false;
}

static bool CursorPos(float& x, float& y) {
    if (!g_hwnd) return false;
    POINT pt;
    RECT rc;
    if (!GetCursorPos(&pt) || !ScreenToClient(g_hwnd, &pt) || !GetClientRect(g_hwnd, &rc)) return false;
    if (rc.right <= 0 || rc.bottom <= 0) return false;
    x = (float)pt.x / rc.right;
    y = (float)pt.y / rc.bottom;
    return x >= 0 && x <= 1 && y >= 0 && y <= 1;
}

static void __fastcall HookIngameGui(void* self, void* frameInfo, void* holder) {
    g_origIngame(self, frameInfo, holder);
    if (!holder || !*(void**)holder || !frameInfo) return;
    if (!g_guiThread) {
        g_guiThread = GetCurrentThreadId();
        Log("gui: in-game GUI pass runs on thread %lu", g_guiThread);
    }
    // Hide while a game menu (e.g. the piste map) is on top, like the game's own HUD.
    bool menu = g_menuOnTop((uint8_t*)frameInfo + 0x238) & 1;
    bool mapOpen = GetTickCount64() - g_lastMapTick < 150;
    static ULONGLONG lastLog = 0;
    if (GetTickCount64() - lastLog > 5000) {
        lastLog = GetTickCount64();
        Log("gui: menuOnTop=%d mapOpen=%d", (int)menu, (int)mapOpen);
    }
    std::lock_guard<std::mutex> lk(g_mu);
    if (menu || mapOpen) {
        Frame f;  // notifications only
        f.showList = false;
        f.toasts = g_frame.toasts;
        f.toastsAtBottom = menu;
        DrawIngame(holder, f);
        return;
    }
    DrawIngame(holder, g_frame);
}

// ---- piste map pass -----------------------------------------------------------
static void __fastcall HookMinimapGui(void* self, void* frameInfo, void* holder, void* p4, void* p5) {
    g_origMinimap(self, frameInfo, holder, p4, p5);
    if (!frameInfo || !holder || !p4 || !p5) return;
    // Same checks the game makes before drawing the map (it returns early when
    // another menu page is up, and our pins would land on that page).
    void* sc = (uint8_t*)frameInfo + kFrameMapScene;
    if (!g_mapPageReady(sc) || !g_mapPageShown(sc)) return;
    g_lastMapTick = GetTickCount64();
    // Pins for everyone skiing on the mountain this map shows (which can be a
    // different one from ours while browsing the other maps).
    const std::string viewed = game::ViewedScene(frameInfo);
    if (viewed.empty()) return;
    static std::string lastViewed;
    if (viewed != lastViewed) {
        lastViewed = viewed;
        Log("map: showing '%s'", viewed.c_str());
    }
    uint8_t* mapState = *(uint8_t**)p4;
    uint8_t* rd = *(uint8_t**)p5;
    if (!mapState || !rd) return;
    mapState += 0x10;  // FUN_14023aba0
    rd += 0x40;        // FUN_14023b150
    void* xf = mapState + 0x1c8;
    void* proj = rd + 0x80;
    if (!*(void**)((uint8_t*)xf + 0x38)) return;  // transform not set up yet
    const float yaw = *(float*)(mapState + 0x4c);

    std::lock_guard<std::mutex> lk(g_mu);
    std::vector<PinHit> pins;
    std::vector<const Player*> shown;
    for (const Player& p : g_frame.players) {
        if (p.you || !p.onMap || p.scene != viewed) continue;
        float mapped[4] = {}, scr[2] = {};
        g_mapXf(xf, mapped, p.feet);
        g_mapProj(proj, scr, mapped);
        // on the visible map only (not under the info panel on the left)
        if (!(scr[0] > kMapLeft && scr[0] < 1.05f && scr[1] > -0.05f && scr[1] < 1.05f)) continue;
        pins.push_back({p.id, scr[0], scr[1]});
        shown.push_back(&p);
    }
    g_pins = pins;
    // The hover ring follows the cursor only while nothing is selected (as the
    // game's does); a pin gets it when closer to the cursor than the point of
    // interest the game would ring. A click then picks what has the ring.
    g_hovered = 0;
    float cx, cy, dPin;
    if (!g_selected && !g_lastPick.selEngaged && CursorPos(cx, cy)) {
        uint32_t id = NearestPin(cx, cy, dPin);
        if (id && g_lastPick.hoverEngaged) {
            float mapped[4] = {}, scr[2] = {};
            g_mapXf(xf, mapped, g_lastPick.hover);
            g_mapProj(proj, scr, mapped);
            float dx = (cx - scr[0]) * g_aspect, dy = cy - scr[1];
            if (sqrtf(dx * dx + dy * dy) <= dPin) id = 0;
        }
        g_hovered = id;
    }
    // (hovered / selected skiers get the game's own hover ring: map_extras)
    for (size_t i = 0; i < pins.size(); ++i) {
        const Player& p = *shown[i];
        const float x = pins[i].x, y = pins[i].y;
        float angle = yaw - atan2f(p.dir[1], p.dir[0]);  // same formula as the game's own pin
        Pin(holder, x, y, 1.f, angle, p.color, 1.f, g_aspect);
        Text(holder, p.name, LabelFont(p.name), x, y - 0.06f, 0.025f, p.color, 0.95f, kAlignCenter, kVAlignTop, 2,
             false);
    }
}

bool Init(uintptr_t base) {
    auto at = [&](uintptr_t rva) { return (void*)(base + rva); };
    if (memcmp(at(kRvaText), kSigText, sizeof kSigText) || memcmp(at(kRvaSprite), kSigSprite, sizeof kSigSprite) ||
        memcmp(at(kRvaMapXf), kSigMapXf, sizeof kSigMapXf) || memcmp(at(kRvaMapProj), kSigMapProj, sizeof kSigMapProj) ||
        memcmp(at(kRvaMenuOnTop), kSigMenuOnTop, sizeof kSigMenuOnTop) || memcmp(at(kRvaImage), kSigImage, sizeof kSigImage)) {
        Log("GUI: signature mismatch, multiplayer UI disabled");
        return false;
    }
    g_text = (TextFn)at(kRvaText);
    g_sprite = (SpriteFn)at(kRvaSprite);
    g_mapXf = (MapXfFn)at(kRvaMapXf);
    g_mapProj = (MapProjFn)at(kRvaMapProj);
    g_menuOnTop = (MenuOnTopFn)at(kRvaMenuOnTop);
    g_image = (ImageFn)at(kRvaImage);
    if (!memcmp(at(kRvaTexUpload), kSigTexUpload, sizeof kSigTexUpload) &&
        !memcmp(at(kRvaTexInsert), kSigTexInsert, sizeof kSigTexInsert) &&
        !memcmp(at(kRvaServices), kSigServices, sizeof kSigServices)) {
        g_texUpload = (TexUploadFn)at(kRvaTexUpload);
        g_texInsert = (TexInsertFn)at(kRvaTexInsert);
        g_services = (ServicesFn)at(kRvaServices);
        g_texCounter = (volatile LONG*)at(kRvaTexCounter);
    } else {
        Log("GUI: texture upload signature mismatch, avatars disabled");
    }
    g_mapPageReady = (MapCheckFn)at(kRvaMapPageReady);
    g_mapPageShown = (MapCheckFn)at(kRvaMapPageShown);
    void** s1 = (void**)at(kSlotMetricTextsGui);
    void** s2 = (void**)at(kSlotMinimapGui);
    bool mapChecks = !memcmp(at(kRvaMapPageReady), kSigMapPageReady, sizeof kSigMapPageReady) &&
                     !memcmp(at(kRvaMapPageShown), kSigMapPageShown, sizeof kSigMapPageShown);
    if (*s1 != at(kRvaMetricTextsGui) || *s2 != at(kRvaMinimapGui) || !mapChecks) {
        Log("GUI: vtable slots unexpected, multiplayer UI disabled");
        return false;
    }
    g_origIngame = (GuiPassFn)*s1;
    g_origMinimap = (MinimapPassFn)*s2;
    hook::WritePtr(s1, (void*)&HookIngameGui);
    hook::WritePtr(s2, (void*)&HookMinimapGui);
    Log("GUI hooks installed (in-game + piste map)");
    return true;
}

void Set(Frame f) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_frame = std::move(f);
}

bool RegisterTexture(void* globals, const std::string& name, int w, int h, const uint8_t* rgba) {
    if (!g_texUpload || !globals || w <= 0 || h <= 0 || !rgba) return false;
    uint8_t* svc = g_services();
    if (!svc || !*(void**)(svc + 0x1b8)) return false;
    uintptr_t ti = *(uintptr_t*)(svc + 0x6b8) & ~(uintptr_t)3;
    if (!ti || strcmp((const char*)ti + 0x10, kGLStateType) != 0) return false;
    void* gl = *(void**)(svc + 0x680);
    if (!gl) return false;
    static bool logged = false;
    if (!logged) {
        logged = true;
        Log("gui: registering textures on thread %lu (GUI thread %lu)", GetCurrentThreadId(), g_guiThread);
    }
    // Same sequence as the title screen's profile picture task.
    alignas(16) uint8_t holder[0x38] = {};
    *(int32_t*)holder = 2;
    *(int32_t*)(holder + 0x30) = InterlockedExchangeAdd(g_texCounter, 1);
    const int wh[2] = {w, h};
    StrView nv{name.data(), name.size()};
    g_texUpload(holder, gl, 0, 2, wh, 0x1401 /*UNSIGNED_BYTE*/, rgba, 0x1908 /*RGBA*/, 0x8058 /*RGBA8*/,
                0x100000de1ULL /*TEXTURE_2D, mipmaps*/, &nv);
    g_texInsert((uint8_t*)globals + kGlobTextureMap, &nv, holder);
    if (holder[0x14]) Log("gui: texture '%s' was not taken by the map", name.c_str());
    return true;
}

void SetAspect(float a) {
    if (a > 0.5f && a < 4.f) g_aspect = a;
}

}  // namespace gui
