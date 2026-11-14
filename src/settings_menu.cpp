#include "settings_menu.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "hook.h"
#include "log.h"

namespace settings_menu {

// ---- game functions (RVAs, found with REA/Ghidra) ----------------------------------
// Hovered-item lookup, called by GameMenuRenderer_Settings::doUpdate right after
// it has rebuilt the item list and before it handles clicks:
//   Out* find_hovered(Out*, vector<Item>* items, Input*)
constexpr uintptr_t kRvaFindHovered = 0x251770;
static const uint8_t kSigFindHovered[16] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0x57, 0x48, 0x81,
                                            0xec, 0xc0, 0x01, 0x00, 0x00, 0x48, 0x8b, 0xf9};
// add_header(Inserter*, float rect[4], float rgba[4], u32string* title)   (moves title)
constexpr uintptr_t kRvaAddHeader = 0x24b5c0;
static const uint8_t kSigAddHeader[16] = {0x4c, 0x8b, 0xdc, 0x53, 0x56, 0x57, 0x48, 0x81,
                                          0xec, 0x10, 0x01, 0x00, 0x00, 0x48, 0x8b, 0x05};
// add_button(Inserter*, Item*, int kind = 1)   (copies the item)
constexpr uintptr_t kRvaAddButton = 0x24b750;
static const uint8_t kSigAddButton[16] = {0x48, 0x8b, 0xc4, 0x48, 0x89, 0x58, 0x10, 0x48,
                                          0x89, 0x70, 0x20, 0x57, 0x48, 0x81, 0xec, 0xa0};
// settingsbutton_render_t* two_choice(out, u32string* label, u32string* a,
//                                     u32string* b, u8 selected)   (consumes the strings)
// e.g. "Reverse turn: (Regular) (Reversed)"
constexpr uintptr_t kRvaTwoChoice = 0x245e90;
static const uint8_t kSigTwoChoice[16] = {0x40, 0x55, 0x53, 0x56, 0x57, 0x41, 0x56, 0x41,
                                          0x57, 0x48, 0x8d, 0xac, 0x24, 0x58, 0xfe, 0xff};
// void* operator_new(size_t)  (the game's allocator; its strings are freed with it)
constexpr uintptr_t kRvaNew = 0x0022e0;
static const uint8_t kSigNew[16] = {0x48, 0x83, 0xec, 0x28, 0x48, 0x81, 0xf9, 0x00,
                                    0x10, 0x00, 0x00, 0x72, 0x2a, 0x48, 0x8d, 0x41};

// Item (0xb0 bytes): rect, colour, render std::function, action std::function, kind
constexpr size_t kItemSize = 0xb0;
constexpr size_t kItemColor = 0x10;   // rgba (header / row text colour)
constexpr size_t kItemRender = 0x20;      // std::function storage (0x40), impl ptr at +0x38
constexpr size_t kItemAction = 0x60;
constexpr size_t kItemKind = 0xa0;        // 0 = box header, 1 = button
constexpr size_t kFuncImplPtr = 0x38;
// The item vector lives at +0x60 of the settings render data; inserters point at +0x10.
constexpr size_t kVecToInserterBase = 0x50;

using FindHoveredFn = void*(__fastcall*)(void* out, uint64_t* vec, void* input, void* r9);
using AddHeaderFn = void(__fastcall*)(uint64_t* inserter, const float* rect, const float* rgba, void* title);
using AddButtonFn = void(__fastcall*)(uint64_t* inserter, void* item, uint64_t kind);
using TwoChoiceFn = void*(__fastcall*)(void* out, void* label, void* a, void* b, uint64_t selected);
using NewFn = void*(__fastcall*)(size_t);

static FindHoveredFn g_origFind;
static AddHeaderFn g_addHeader;
static AddButtonFn g_addButton;
static TwoChoiceFn g_twoChoice;
static NewFn g_new;
static std::function<State()> g_getState;
static std::function<void(Row)> g_onClick;
static volatile ULONGLONG g_lastShown = 0;

double SecondsSinceShown() { return g_lastShown ? (GetTickCount64() - g_lastShown) / 1000.0 : 1e9; }

// ---- MSVC std::u32string built with the game's allocator ------------------------
struct U32String {
    union {
        char32_t buf[4];
        char32_t* ptr;
    };
    size_t size;
    size_t cap;
};

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

static void MakeString(U32String* out, const std::string& utf8) {
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

// ---- std::function implementations (MSVC _Func_base layout) ------------------------
// vtable: _Copy(where), _Move(where), _Do_call(...), _Target_type(), _Delete_this(bool), _Get()
struct Impl {
    void* const* vtbl;
    int row;
};
static_assert(sizeof(Impl) <= 0x38, "must fit std::function's small buffer");

static void* __fastcall ImplCopy(Impl* self, void* where) {
    memcpy(where, self, sizeof(Impl));
    return where;
}
static void __fastcall ImplDelete(Impl*, uint64_t) {}
static const void* __fastcall ImplTargetType(Impl*) {
    static const uint64_t kNoType[8] = {};
    return kNoType;
}
static void* __fastcall ImplGet(Impl* self) { return (uint8_t*)self + 8; }

static void* __fastcall RenderCall(Impl* self, void* ret, void*, void*, void*) {
    State st = g_getState();
    std::string label, a, b;
    int sel = 0;
    switch (self->row) {
        case kRowMode: label = "Mode:", a = "Host", b = "Join", sel = st.host ? 0 : 1; break;
        case kRowAddress:
        case kRowPort: {
            label = self->row == kRowPort ? "Port:" : st.host ? "Host address (when joining):" : "Host address:";
            if (st.editRow == self->row) {  // typing: text + blinking caret
                a = st.editText + ((GetTickCount64() / 500) % 2 ? "_" : " ");
                b = "Enter to save, Esc to cancel";
            } else {
                a = self->row == kRowPort ? std::to_string(st.port) : st.address.empty() ? "-" : st.address;
                b = "Edit";
            }
            sel = 0;
            break;
        }
        default: label = "Name tags:", a = "On", b = "Off", sel = st.names ? 0 : 1; break;
    }
    U32String l, x, y;
    MakeString(&l, label);
    MakeString(&x, a);
    MakeString(&y, b);
    return g_twoChoice(ret, &l, &x, &y, (uint64_t)sel);
}

static void __fastcall ActionCall(Impl* self, void*, void*, void*) {
    if (self->row >= 0 && self->row < kRowCount) g_onClick((Row)self->row);
}

static void* const kRenderVtbl[6] = {(void*)&ImplCopy, (void*)&ImplCopy, (void*)&RenderCall,
                                     (void*)&ImplTargetType, (void*)&ImplDelete, (void*)&ImplGet};
static void* const kActionVtbl[6] = {(void*)&ImplCopy, (void*)&ImplCopy, (void*)&ActionCall,
                                     (void*)&ImplTargetType, (void*)&ImplDelete, (void*)&ImplGet};

// ---- layout ------------------------------------------------------------------------------
struct Rect {
    float l, t, r, b;
};
static Rect RectAt(const uint8_t* p) {
    Rect r;
    memcpy(&r, p, sizeof r);
    return r;
}

// A box = a header item plus the rows below it in the same column (the game's
// boxes have no separate background rect; the column + header define them).
struct Box {
    Rect header;
    uint8_t headerColor[16];
    std::vector<Rect> rows;
    uint8_t rowColor[16];
};

// Our box goes at the bottom of the left-hand column, below its last row, with
// the same header size, row height, row spacing and gap between boxes as the
// game uses.
static bool Layout(const std::vector<Box>& boxes, int rows, Rect& header, std::vector<Rect>& out, const Box*& ref) {
    ref = nullptr;
    for (const Box& b : boxes) {
        if (b.rows.empty()) continue;
        if (!ref || b.header.l < ref->header.l - 0.05f ||
            (fabsf(b.header.l - ref->header.l) <= 0.05f && b.header.t > ref->header.t))
            ref = &b;
    }
    if (!ref) return false;
    const Rect& r0 = ref->rows.front();
    const float rowH = r0.b - r0.t;
    const float pitch = ref->rows.size() > 1 ? ref->rows[1].t - r0.t : rowH * 1.1f;
    const float firstOff = r0.t - ref->header.t;
    // Gap from a box's last row to the next box's header, measured in any column
    // that stacks two boxes; else two row pitches.
    float gap = 2 * pitch;
    for (const Box& a : boxes)
        for (const Box& b : boxes)
            if (&a != &b && !a.rows.empty() && fabsf(a.header.l - b.header.l) <= 0.05f && b.header.t > a.header.t) {
                float g = b.header.t - a.rows.back().b;
                if (g > 0 && g < 0.2f) gap = g;
            }
    float top = ref->rows.back().b + gap;
    header = {ref->header.l, top, ref->header.r, top + (ref->header.b - ref->header.t)};
    out.clear();
    for (int i = 0; i < rows; ++i) {
        float t = top + firstOff + i * pitch;
        out.push_back({r0.l, t, r0.r, t + rowH});
    }
    return true;
}

static void LogItems(const std::vector<Box>& boxes) {
    for (const Box& b : boxes) {
        Log("settings: header (%.3f %.3f %.3f %.3f) rows=%zu", b.header.l, b.header.t, b.header.r, b.header.b,
            b.rows.size());
        for (const Rect& r : b.rows) Log("settings:   row (%.3f %.3f %.3f %.3f)", r.l, r.t, r.r, r.b);
    }
}

static void Append(uint64_t* vec) {
    uint8_t* begin = (uint8_t*)vec[0];
    uint8_t* end = (uint8_t*)vec[1];
    if (!begin || end <= begin || (size_t)(end - begin) % kItemSize) return;
    std::vector<Box> boxes;
    std::vector<std::pair<Rect, const uint8_t*>> rowItems;
    for (uint8_t* it = begin; it < end; it += kItemSize) {
        if (*(void* const**)(it + kItemAction) == kActionVtbl) return;  // already added this update
        int kind = *(int*)(it + kItemKind);
        if (kind == 0) {
            Box b{RectAt(it), {}, {}, {}};
            memcpy(b.headerColor, it + kItemColor, 16);
            boxes.push_back(b);
        } else if (kind == 1) {
            rowItems.push_back({RectAt(it), it});
        }
    }
    // Each row belongs to the nearest header above it in the same column.
    for (auto& [r, it] : rowItems) {
        Box* owner = nullptr;
        for (Box& b : boxes)
            if (fabsf(b.header.l - r.l) <= 0.05f && b.header.t <= r.t && (!owner || b.header.t > owner->header.t))
                owner = &b;
        if (!owner) continue;
        if (owner->rows.empty()) memcpy(owner->rowColor, it + kItemColor, 16);
        owner->rows.push_back(r);
    }
    for (Box& b : boxes)
        std::sort(b.rows.begin(), b.rows.end(), [](const Rect& x, const Rect& y) { return x.t < y.t; });

    static ULONGLONG lastSeen = 0;
    bool opened = GetTickCount64() - lastSeen > 2000;
    lastSeen = GetTickCount64();
    g_lastShown = lastSeen;
    if (opened) LogItems(boxes);

    Rect header;
    std::vector<Rect> rows;
    const Box* ref = nullptr;
    if (!Layout(boxes, kRowCount, header, rows, ref)) return;
    if (opened) Log("settings: multiplayer header (%.3f %.3f %.3f %.3f)", header.l, header.t, header.r, header.b);

    uint64_t base = (uint64_t)vec - kVecToInserterBase;
    uint64_t headerInserter[9] = {base};  // + an empty std::function (no action)
    alignas(16) U32String title;
    MakeString(&title, "Multiplayer");
    // (the game loads these with aligned SSE moves: 16-byte alignment required)
    alignas(16) float headerRect[4], headerColor[4];
    memcpy(headerRect, &header, 16);
    memcpy(headerColor, ref->headerColor, 16);
    g_addHeader(headerInserter, headerRect, headerColor, &title);

    static uint8_t noJoypad[0x40] = {};
    uint64_t buttonInserter[2] = {base, (uint64_t)noJoypad};
    for (int i = 0; i < kRowCount; ++i) {
        alignas(16) uint8_t item[kItemSize] = {};
        memcpy(item, &rows[i], sizeof(Rect));
        memcpy(item + kItemColor, ref->rowColor, 16);
        Impl render{kRenderVtbl, i}, action{kActionVtbl, i};
        memcpy(item + kItemRender, &render, sizeof render);
        *(void**)(item + kItemRender + kFuncImplPtr) = item + kItemRender;
        memcpy(item + kItemAction, &action, sizeof action);
        *(void**)(item + kItemAction + kFuncImplPtr) = item + kItemAction;
        g_addButton(buttonInserter, item, 1);
        // (our impls own nothing, so the temporary needs no destruction)
    }
}

static void* __fastcall HookFindHovered(void* out, uint64_t* vec, void* input, void* r9) {
    if (vec) Append(vec);
    return g_origFind(out, vec, input, r9);
}

bool Init(uintptr_t base, std::function<State()> getState, std::function<void(Row)> onClick) {
    auto at = [&](uintptr_t rva) { return (void*)(base + rva); };
    if (memcmp(at(kRvaAddHeader), kSigAddHeader, 16) || memcmp(at(kRvaAddButton), kSigAddButton, 16) ||
        memcmp(at(kRvaTwoChoice), kSigTwoChoice, 16) || memcmp(at(kRvaNew), kSigNew, 16)) {
        Log("Settings menu: signature mismatch, multiplayer settings box disabled");
        return false;
    }
    g_addHeader = (AddHeaderFn)at(kRvaAddHeader);
    g_addButton = (AddButtonFn)at(kRvaAddButton);
    g_twoChoice = (TwoChoiceFn)at(kRvaTwoChoice);
    g_new = (NewFn)at(kRvaNew);
    g_getState = std::move(getState);
    g_onClick = std::move(onClick);
    g_origFind = (FindHoveredFn)hook::Detour(at(kRvaFindHovered), kSigFindHovered, sizeof kSigFindHovered,
                                             (void*)&HookFindHovered);
    if (!g_origFind) {
        Log("Settings menu: hook failed, multiplayer settings box disabled");
        return false;
    }
    Log("Settings menu hook installed");
    return true;
}

}  // namespace settings_menu
