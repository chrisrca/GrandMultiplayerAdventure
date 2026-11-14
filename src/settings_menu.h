// A "Multiplayer" box on the game's own Settings screen.
//
// The Settings screen (GameMenuRenderer_Settings) rebuilds a list of items every
// update: one header item per box ("Graphics", "Audio", ...) and one button item
// per row. Each button carries two MSVC std::function objects - render (returns
// the row's "Label: (A) (B)" text) and action (runs on click). We append our own
// header + buttons to that list right before the game looks for the hovered /
// clicked item, so the game lays out, draws, highlights and clicks them exactly
// like its own rows.
#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace settings_menu {

enum Row : int {
    kRowMode,      // Mode: (Host) (Join)
    kRowAddress,   // Host address: (1.2.3.4) (Edit)      - click to type
    kRowPort,      // Port: (47800) (Edit)                - click to type
    kRowNames,     // Name tags: (On) (Off)
    kRowCount
};

struct State {
    bool host = true;
    std::string address;
    int port = 47800;
    bool names = true;
    int editRow = -1;       // row being typed into, or -1
    std::string editText;
};

// getState is called from the game thread while the Settings screen is open;
// onClick is called from the game thread when a row is clicked. Both must be
// thread-safe with respect to the rest of the mod.
bool Init(uintptr_t exeBase, std::function<State()> getState, std::function<void(Row)> onClick);

// Seconds since the Settings screen was last on screen (large if never).
double SecondsSinceShown();

}  // namespace settings_menu
