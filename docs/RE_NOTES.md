# Reverse-engineering notes

Target: `gma_wonderlands.exe`, Steam build, SHA-256 `766927fdbe20e687d85e99b3ed988eac8cf50f782644363812561f1f73629011`, image base `0x140000000`. It's a custom C++ engine (MSVC, SDL2, OpenGL through GLEW). It is not Unity, so there is no managed code to hook.

Tools: REA 6.5.0 with Ghidra 12.1.4 for static analysis, and REA's Frida provider for runtime confirmation. All RVAs below are relative to the image base.

## Loading

The exe statically imports `zlib.dll` and uses only `inflate`, `inflateInit2_` and `inflateEnd`. The mod ships as `zlib.dll`, whose `.def` file forwards all 84 exports to `zlib_orig.dll`. `SDL_GL_SwapWindow` is hooked through the exe's import address table.

## Player position

- `ns_player::Player::GetSkiDirectionXY(Player const*, Vec2*)` is at RVA `0x3dcdc0`. It was found through the `Player.cpp` assert path string and the function-name string inside it.
- At runtime (Frida), it is called about 4 times per frame with a single `Player*`.
- `Player + 0x310`: `Vec3<double>` feet. `+0x328`: mid-body. `+0x340`: head. The world is **Z-up**, matching the "XY" in the function name.
- Players array: `*(SessionInfo + 0x48)`, stride `0x10d0`, from `FUN_140474e10`. At runtime, element 0 equals the pointer passed to `GetSkiDirectionXY`.

## Per-frame game-thread hook

- `game::handle_scene_max(SessionInfo const&, SessionMutator&)` is at RVA `0x1a1fb0`.
- How it was found: the RTTI type descriptor for `boost::any::holder<maxstates_data_t (local to handle_scene_max)>` at `0x1407a00c0` leads to the complete object locator `0x1406f7bb8`. That leads to the vtable `0x1406c2390`, which is referenced only from `0x1401a1fb0`.
- Frida: about 1 call per frame, always with the same `SessionInfo` and `SessionMutator`.
- The 14-byte prologue (`48 8b c4 48 89 58 18 55 56 57 41 54 41 55`) is position-independent and is detoured with an absolute jump.

## Scenes

- `SessionInfo + 0x258`: current scene index. `FUN_1402d85a0(SessionInfo+0x258)` (RVA `0x2d85a0`) returns the index including a queued change.
- `SessionInfo + 0x2d0` and `SessionMutator + 0x50` are both `GMAGlobals**`, and point to the same object at runtime.
- `*(GMAGlobals + 0x40) + 0x1dba8 / 0x1dbb0`: `vector<SceneHeader>`, element size `0x70`. An MSVC `std::string` internal name sits at `+0x08`.
- The 12 scenes, by index: hirschalm, waldtal, elnakka, dalarna, gaupefjord, rotkamm, saint_luvette, passo_grolla, ben_ailig, charyn_canyon, mount_fairview, pinecone_peaks.

## Travel

- `game::goto_worldplace` is at RVA `0x2bd830`, identified by the `"game::goto_worldplace"` assert string in LoadSceneUtil.cpp. Its signature: `(GMAGlobals+0xd20, u8 mode, SessionInfo*, SessionMutator*, size_t sceneIdx, opt*, opt*, opt*, opt*)`.
- The mod calls it the same way the game's own command functor `FUN_140068170` does (vtable `0x1406b47a0`): mode `3` and four empty optionals.
- Confirmed with Frida: calling it from inside `handle_scene_max` changed the scene index from 1 to 0, and the game loaded Hirschalm normally.
- Downstream, `load_scene_func` (`0x2bcd10`) applies the game's own lock and DLC checks.

## Drawing remote players with the game's own character renderer

The game draws every skier from a **ghost frame**: the local player, recorded ghosts, and online-multiplayer clients.

- **Ghost frame layout.** 0x418 bytes: up to 26 bones of 0x28 bytes each, with the count at `+0x410`; it is copied with `memcpy`. Each bone is `{start xyz, end xyz, dir xyz}` as world-space floats, plus a 4-byte **integer body-part tag**. For a skier the tags are `1 1 8 8 7 7 6 5 11 9 9 10 10 12 12 13 13 4 4` (19 bones). The tag must be copied bit-exact. Quantizing it as a float collapses every bone into the same thin part.
- **`make_ghost_frame(out, Player const*, float extrapolate)`**, RVA `0x14a480`. It is used by `Handler_RealtimeGhostframe` (vtable `0x1406b5ac8`; its update at `0x140084720` keeps the last 16 frames per player at `sessionSub+0x4d8`). The character renderer also calls it, with 0.04.
- **Character render-data builder** `0x390820`, reached through the vtable slot at `0x1406d9dc8` (the RTTI of `holder<CharacterRenderData>` leads there). It runs once per frame on the main thread. For each local player it calls the draw routine with the player's frame, and for each online client it does:
  `draw_character(frame, models[client.modelIdx], frameInfo[5], lod = *(*(G+0x40)+0x1d668), 0.02f, hue = (uid%1000)/1000, &clientColor, &zero, &tint{flag=0}, (**renderData)+0xa8, nullptr, nullptr)`
  - `draw_character` is at RVA `0x111200`.
  - The character models are at `*(GMAGlobals+0x1230)`, 0x18 bytes each.
- **The mod's injection.** It replaces that vtable slot. After the original builder runs, the mod calls `draw_character` exactly like the online-client branch, passing the remote player's frame and that player's own hue.
- **Hue.** `get_character_color_hue(GMAGlobals+0xd30)` at `0x2d70d0` reads the `character_color_hue` setting, a float in [-1, 1]. The local player is drawn with this value, so sending it makes remote players look the way they customized themselves.
- **Verified live:** a Frida-injected copy of the local frame, offset 4 m, rendered as a fully shaded second skier.

### Dead ends (kept for reference)

- **Native online multiplayer.** `Handler_OnlineMultiplayer` (`0x1401d3180`), `ns_network::room_handler`, and the `udp_util` listeners for `internal_room` / `clients_unreliable` are all present. But the PC build has no transport: no Winsock import, and the self-IP function `0x28d0b0` always reports invalid. Forcing the mode bytes (`GMAGlobals+0x13f8 = 2`, `*(SessionInfo+0x20)+0x800 = 2`) crashed the game inside room setup.
- **`multiplayer_renderdata_t`** is an empty dummy. Remote clients are synced into `Character_MultiplayerFrameProxy` entities (`FUN_14028d790`), but they are drawn through the builder above.

## Camera

- Uniforms are looked up by name at runtime; the name table is at `0x140654000`. The main pass uploads `uViewProjectionMatrix` through `__glewUniformMatrix4fv` once per shader program.
- The mod wraps glew's `__glewUseProgram`, `__glewUniformMatrix4fv` and `__glewDeleteProgram` pointers. For each program it looks up the `uViewProjectionMatrix` location itself, and keeps the matrix uploaded with the largest viewport that matches the screen's aspect ratio. This rejects the 2046×2046 shadow pass.
- Frida check: projecting the player position through the captured matrix gives NDC (−0.01, −0.38), just below screen center, as expected for a chase camera. `uCameraPosition` is about 50 units above and behind the player.

The camera matrix is now used only to place name labels and edge pins (see "Game GUI").

## Game GUI (text, sprites, piste map)

All of the mod's UI goes through the game's own GUI renderer. The calls were captured live with Frida and checked on screen.

- **Text: `0x1fb800`.** Signature: `gui_text(GuiList*, u32string_view* text, string_view* font, u64 pos, float size, float* rgba, u8 style, u8 valign, u8 layer, u8 halign, u64* shadow)`.
  - Text is UTF-32.
  - `font` is a font name: `"staatliches"` (uppercase labels) or `"contrailone"` (italic titles).
  - `pos` is two packed floats, normalized 0–1 from the top-left.
  - `size` is a fraction of screen height.
  - `halign`: 3 = left, 2 = center. 0 also renders left-aligned in our calls.
  - `valign`: 1 = middle, 3 = top (the text hangs below `pos`). Measured from the returned text bounds.
  - `shadow = 0x100000001` turns on the drop shadow used by "Next Mountain".
- **Sprite: `0x1fc1e0`.** Signature: `gui_sprite(GuiList** holder, string_view* texture, float rect[4], float rgba[4], u8 layer, u8 flag, float angle, float uv[4])`.
  - `rect` is (left, top, right, bottom), normalized.
  - Textures are looked up by name; `_r` textures are single-channel masks tinted by `rgba`.
  - `angle` is in radians, clockwise from "up".
- **Where the mod hooks in.** It appends to two GUI passes by replacing vtable slot 3 of each class: `(self, FrameInfo*, GuiList** holder [, ...])`.
  - `Handler_MetricTexts` (slot `0x1406c34e0` → `0x1401d0840`): runs every gameplay frame.
  - `GameMenuRenderer_MinimapModern` (slot `0x1406c8088` → `0x14022f5f0`): runs while the piste map is open.
- **Menu check.** `menu_on_top(FrameInfo+0x238)` (`0x26ff40`) is true while a game menu is on top. The in-game list and labels are hidden then.
- **Piste-map player pin.**
  - Composition: `playerdir_r` (the ring with pointer, rotated) and `playerpos_r` (the avatar). Both are layer 3 with flag 3, plus a black 0.2-alpha copy 1.25× larger on layer 1 as the shadow.
  - Size, as a fraction of screen height: 0.096 for the ring, 0.0587 for the avatar.
  - Angle: `mapYaw − atan2(dir.y, dir.x)`, where `mapYaw` is the float at `mapState+0x4c`.
  - Projection:
    - `mapState = *(arg4)+0x10`.
    - World → map space: `FUN_1400ecfb0(mapState+0x1c8, out, world)`.
    - Map space → normalized screen: `FUN_14050b060(*(arg5)+0x40+0x80, out, map)`.
- **Map labels** ("MAX JUMP HEIGHT 8.1M"): `staatliches`, size 0.025, colors such as (0.8, 0.6, 1.0, 0.95), style 2, valign 3, halign 2.
- **Controls screen** (`GameMenuRenderer_Controls` GUI pass `0x140043930`):
  - Its dark band is `glyph_gradient_square_r` on layer 2.
  - Titles ("Player 1") are `contrailone`; option labels are `staatliches`.
- **Image (untinted texture): `0x1fbf90`.** Signature: `gui_image(GuiList** holder, string_view* texture, float rect[4], float alpha, u8 layer, u8 flag, u64 0, float uv[4])`. The title screen draws your Steam picture with it.
- **Text bounds.** `gui_text` returns the laid-out text. Its pixel bounds are floats at `+0x400` (left, top, right, …) and the screen size at `+0x420/+0x424`. The title screen uses the right edge to place the version number after your name.
- **Snowboards** need no special handling. The ghost frame carries the board bones, and the remote skier renders on the correct gear.

## Steam name and avatar (title screen)

- `CloudService_Steam` (CloudService_Steam.cpp) uses the versioned interfaces `SteamFriends017`, `SteamUser021` and `SteamUtils010`. The game never calls `SteamAPI_RunCallbacks`. The mod uses the same interfaces through `steam_api64.dll`'s flat API (`SteamAPI_SteamFriends_v017`, `RequestUserInformation`, `GetMediumFriendAvatar`, `GetImageRGBA`, …) and pumps callbacks only while an avatar is still downloading.
- **Upload.** `FUN_14025c3f0` (Splash.cpp, the title screen) gets the profile image, then queues a `std::function<void(GMAGlobals&, portis::env&, gl::GLBigState&)>` GL task: lambda `_Do_call` `0x1402600a0` → `FUN_14025dc60`. That task:
  1. builds a 0x38-byte texture holder: int `2` at `+0`, zeros, and a serial from the counter `DAT_14078c0d0` at `+0x30`;
  2. calls `gl::GLTextureHolder::upload_impl` (`0x520db0`) with `(holder, GLBigState*, 0, 2, int wh[2], 0x1401, pixels, 0x1908, 0x8058, 0x1'00000de1, &name)`;
  3. inserts it with `FUN_140505d20(GMAGlobals+0x48, &name, holder)` into the named-texture map. The map is a sorted pair of vectors (names `std::string`, holders 0x38 bytes), and an existing name is replaced.
- **GLBigState.** The service registry `FUN_1404f1a40()` (a function-local static) holds a `std::shared_ptr<gl::GLBigState>` at `+0x680`. Its type_info pointer is at `+0x6b8`, with the low 2 bits used as flags, and the decorated name is `.?AV?$shared_ptr@VGLBigState@gl@@@std@@`. The registry is ready when `+0x1b8` is non-zero. The game's own clean-up path reads it the same way.
- **The mod** runs the same three steps on the GL thread (the swap hook) for each player's SteamID, under the name `gmamp_av_<steamid>`.
- **Drawing** (`FUN_14025e390`):
  - The avatar is drawn with `gui_image` on layer 2 with flag 1. The rect is 0.025 of screen height, square, and scaled by 0.8.
  - Behind it, `glyph_gradient_circle_r` is drawn at 1.5× size, white at half alpha, on layer 1 with flag 1.
  - The name is in `mplus1` at size 0.025. This font also covers non-Latin names.

## Settings screen (Settings.cpp)

- `GameMenuRenderer_Settings::doUpdate` is `0x2471b0`, with the signature `(this, SessionInfo*, SessionMutator*, ?, bool* handled, RenderData*)`. Its data block is `C = **(arg6+0x10)`.
- **The item list is rebuilt on every update.** It is `vector<Item>` at `C+0x60`. Each `Item` is 0xb0 bytes:
  - `+0x00` rect (l, t, r, b, normalized)
  - `+0x10` rgba colour (header / row text)
  - `+0x20` render `std::function<settingsbutton_render_t(SessionInfo const&, PreferencesLocal const&, SettingsState const&)>`
  - `+0x60` action `std::function<void(SessionInfo const&, SessionMutator&, PreferencesLocal&)>`
  - `+0xa0` int kind: 0 = box header, 1 = button
- **Boxes.** A box is a header item ("Game", "Audio", "Graphics", …) followed by button rows in the same column. There is no separate background rect; the dark bands come from the screen's own layout.
  - Headers are added with `add_header(inserter, rect, rgba, u32string* title)` at `0x24b5c0`. The inserter is `{C+0x10, std::function (empty)}`. The function loads `rect` and `rgba` with `MOVAPS`, so both must be 16-byte aligned; an unaligned pointer crashes the game.
  - Buttons are added with `add_button(inserter, Item*, kind)` at `0x24b750`. The inserter is `{C+0x10, nav*}`, and `nav+0x18` is the joypad navigation graph (null skips it).
- **Row text.** `two_choice(out, u32string* label, u32string* a, u32string* b, u8 selected)` at `0x245e90` builds "Reverse turn: (Regular) (Reversed)". It consumes all three strings, which must be allocated with the game's `operator new` (`0x0022e0`). The general builder is `0x245b30`, which takes small-vectors of options and of selected indices.
- **Clicks.** Right after rebuilding the list, doUpdate calls `find_hovered(out, &items, input)` at `0x251770`. For the item under the cursor, if `kind == 1` and it has an action, doUpdate then calls the action.
- **std::function ABI (MSVC).** The storage is 0x40 bytes, and the impl pointer is at `+0x38`; it points into the storage for small callables. The vtable is `_Copy(where)`, `_Move(where)`, `_Do_call(...)`, `_Target_type()`, `_Delete_this(bool)`, `_Get()`. The render `_Do_call` is `(self, ret*, session, prefs, state)` and returns `ret`.
- **The mod's injection.** The mod detours `find_hovered` (16-byte prologue). It appends a "Multiplayer" header and its buttons through the two inserters, using `std::function` impls whose vtables point at the mod's own functions. The game then lays out, draws, highlights and clicks them like its own rows. The box goes below the last row of the left column ("Game"). It copies the game's header height, row height and spacing, and uses the gap between "Audio" and "Graphics" before its header.
- **Text entry.** The mod subclasses the game's SDL window procedure. While a row is being typed into, it consumes `WM_KEYDOWN`/`WM_CHAR`, so SDL never sees those keys and Esc doesn't leave the screen.

## Render pipeline (for reference)

Traced per frame with Frida:

| FBO | Pass |
|---|---|
| 6 | Shadow map (2048²) |
| 2 | Main scene (1920×1080). Color is RGBA8. Depth is a 32-bit texture with `GL_LESS`, cleared to 1.0, depth range [0, 1]. |
| 3, 4, 5 | Post-processing |
| 0 | Composite + HUD |

## Gotchas found while testing

- The game can leave a `GL_PIXEL_PACK_BUFFER` bound at swap time. The frame dump unbinds it, or `glReadPixels` writes into that buffer.
- GDI screen capture of the fullscreen GL window returns a stale frame. `gma_mp_shot.req` makes the mod dump its own framebuffer instead.
- In fullscreen, the game minimizes itself when it loses focus.
- While minimized, SDL reports a 1280×720 drawable even though the game still renders at 1920×1080. The mod therefore takes the aspect ratio from the game's last viewport.
