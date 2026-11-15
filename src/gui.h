// All multiplayer UI is drawn through the game's own GUI renderer: its text
// renderer with its fonts (staatliches / contrailone) and its sprite renderer
// with its textures (piste-map player pin "playerdir_r" + "playerpos_r",
// "glyph_gradient_square_r" menu bands). Nothing here is custom-rendered.
// Steam avatars are uploaded through the game's own texture holder and named-
// texture map (exactly how the title screen shows your Steam picture) and then
// drawn with the same calls the title screen uses.
//
//  * In game: hooked into Handler_MetricTexts' GUI pass -> name labels over
//    other skiers, edge-of-screen pins for off-screen skiers, the player list
//    (Controls-screen style, middle-left, Steam avatar + name per row) and
//    join/leave notifications.
//  * Piste map: hooked into GameMenuRenderer_MinimapModern -> a pin + label for
//    every remote player on the map, projected with the map's own transform.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace gui {

struct Player {
    uint32_t id = 0;         // network id (0 for the local player)
    std::string name;
    std::string avatar;      // registered texture name ("" = no Steam avatar yet)
    float color[3] = {1, 1, 1};
    bool you = false;
    bool host = false;
    std::string status;      // right-hand text in the list ("42M", "WALDTAL", ...)
    bool dim = false;        // not on our mountain / not in world
    // Map data (remote players skiing on any mountain)
    bool onMap = false;
    std::string scene;            // internal name of their mountain
    // World data (remote players on our mountain only)
    bool visible = false;
    float anchor[3] = {0, 0, 0};  // top of the head (label anchor)
    std::vector<float> outline;   // bone end points (xyz...): label goes above all of them on screen
    float feet[3] = {0, 0, 0};
    float dir[2] = {1, 0};
    float dist = 0;
};

struct Toast {
    std::string text;
    std::string avatar;      // optional avatar texture shown left of the text
    float alpha;
};

struct Frame {
    std::vector<Player> players;   // list order; local player first
    std::string header;            // "MULTIPLAYER"
    std::string subline;           // connection status
    bool subGood = true;
    bool showList = true;          // toggled with toggleKey
    std::string toggleKey = "F8";
    bool showNames = true;
    float viewProj[16] = {};       // gameplay camera (column-major, GL clip)
    bool viewProjValid = false;
    std::vector<Toast> toasts;
    bool toastsAtBottom = false;   // a menu is open: keep clear of its content
};

bool Init(uintptr_t exeBase);
void Set(Frame f);
void SetAspect(float widthOverHeight);

// Uploads an RGBA8 image as a game texture under `name` (replaces an existing
// one), usable by the sprite calls above. Must run on the GL thread (the swap
// hook). `globals` is the game's GMAGlobals*. Returns false if the game isn't
// ready yet.
bool RegisterTexture(void* globals, const std::string& name, int w, int h, const uint8_t* rgba);

// ---- piste map: picking other skiers -------------------------------------------
// The game window (cursor position for hovering the map pins).
void SetWindow(void* hwnd);
// A left click at normalised window coordinates, on the piste map handled the
// way the game handles clicks on its points of interest:
//  - nothing selected: a click picks what has the hover ring; when that is a
//    skier, they are selected (Go to <name>) and the camera zooms to them;
//  - a skier selected: a click (not on the action button) deselects them and
//    the camera goes back;
//  - the game's own selection: the click is the game's.
// Returns true when the click was ours (the game must not see it).
bool MapClick(float x, float y);
// What the game has in a map update (from the map update, after the game):
// its selection (selChanged = changed in this update) and where it puts its
// hover ring (the selection, else the point of interest nearest the cursor).
struct GamePick {
    bool selChanged;
    bool selEngaged;
    float sel[3];
    bool hoverEngaged;
    float hover[3];
};
void NoteGamePick(const GamePick& g);
// What the map camera should do after a click on / off a skier (once).
enum CameraAction { kCamNone, kCamZoomToSelected, kCamBack };
CameraAction TakeCameraAction();
// The selected skier's id (0 = none). Cleared when the map closes.
uint32_t Selected();
// The skier whose pin is under the cursor on the piste map and closer to it
// than the game's hovered point of interest (0 = none).
uint32_t HoveredPin();
void ClearSelection();
// Latest frame data for a player by id.
bool GetPlayer(uint32_t id, Player& out);

}  // namespace gui
