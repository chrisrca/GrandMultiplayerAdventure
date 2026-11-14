#include "steam.h"

#include <windows.h>

#include <set>

#include "log.h"

namespace steam {

using AccessorFn = void*(__cdecl*)();
using GetSteamIdFn = uint64_t(__cdecl*)(void*);
using GetPersonaNameFn = const char*(__cdecl*)(void*);
using GetFriendPersonaNameFn = const char*(__cdecl*)(void*, uint64_t);
using RequestUserInfoFn = bool(__cdecl*)(void*, uint64_t, bool);
using GetAvatarFn = int(__cdecl*)(void*, uint64_t);
using GetImageSizeFn = bool(__cdecl*)(void*, int, uint32_t*, uint32_t*);
using GetImageRGBAFn = bool(__cdecl*)(void*, int, uint8_t*, int);
using RunCallbacksFn = void(__cdecl*)();

static bool g_tried = false, g_ok = false;
static void *g_friends, *g_user, *g_utils;
static GetSteamIdFn pGetSteamId;
static GetPersonaNameFn pGetPersonaName;
static GetFriendPersonaNameFn pGetFriendPersonaName;
static RequestUserInfoFn pRequestUserInfo;
static GetAvatarFn pGetMediumAvatar;
static GetImageSizeFn pGetImageSize;
static GetImageRGBAFn pGetImageRGBA;
static RunCallbacksFn pRunCallbacks;
static std::set<uint64_t> g_requested;

static bool Init() {
    if (g_tried) return g_ok;
    g_tried = true;
    HMODULE m = GetModuleHandleA("steam_api64.dll");
    if (!m) {
        Log("Steam: steam_api64.dll not loaded");
        return false;
    }
    auto get = [&](const char* n) { return (void*)GetProcAddress(m, n); };
    auto friendsAcc = (AccessorFn)get("SteamAPI_SteamFriends_v017");
    auto userAcc = (AccessorFn)get("SteamAPI_SteamUser_v021");
    auto utilsAcc = (AccessorFn)get("SteamAPI_SteamUtils_v010");
    pGetSteamId = (GetSteamIdFn)get("SteamAPI_ISteamUser_GetSteamID");
    pGetPersonaName = (GetPersonaNameFn)get("SteamAPI_ISteamFriends_GetPersonaName");
    pGetFriendPersonaName = (GetFriendPersonaNameFn)get("SteamAPI_ISteamFriends_GetFriendPersonaName");
    pRequestUserInfo = (RequestUserInfoFn)get("SteamAPI_ISteamFriends_RequestUserInformation");
    pGetMediumAvatar = (GetAvatarFn)get("SteamAPI_ISteamFriends_GetMediumFriendAvatar");
    pGetImageSize = (GetImageSizeFn)get("SteamAPI_ISteamUtils_GetImageSize");
    pGetImageRGBA = (GetImageRGBAFn)get("SteamAPI_ISteamUtils_GetImageRGBA");
    pRunCallbacks = (RunCallbacksFn)get("SteamAPI_RunCallbacks");
    if (!friendsAcc || !userAcc || !utilsAcc || !pGetSteamId || !pGetPersonaName || !pGetFriendPersonaName ||
        !pRequestUserInfo || !pGetMediumAvatar || !pGetImageSize || !pGetImageRGBA) {
        Log("Steam: flat API entry points missing");
        return false;
    }
    g_friends = friendsAcc();
    g_user = userAcc();
    g_utils = utilsAcc();
    g_ok = g_friends && g_user && g_utils;
    if (g_ok) Log("Steam: signed in as '%s' (%llu)", LocalName().c_str(), (unsigned long long)LocalId());
    else Log("Steam: interfaces unavailable (not running through Steam?)");
    return g_ok;
}

bool Available() { return Init(); }
uint64_t LocalId() { return Init() ? pGetSteamId(g_user) : 0; }

std::string LocalName() {
    if (!Init()) return "";
    const char* n = pGetPersonaName(g_friends);
    return n ? n : "";
}

std::string Name(uint64_t id) {
    if (!id || !Init()) return "";
    if (g_requested.insert(id).second) pRequestUserInfo(g_friends, id, false);
    const char* n = pGetFriendPersonaName(g_friends, id);
    if (!n || !*n || strcmp(n, "[unknown]") == 0) return "";
    return n;
}

bool GetAvatar(uint64_t id, Avatar& out) {
    if (!id || !Init()) return false;
    if (g_requested.insert(id).second) pRequestUserInfo(g_friends, id, false);
    int img = pGetMediumAvatar(g_friends, id);
    if (img <= 0) return false;  // 0 = none, -1 = still loading
    uint32_t w = 0, h = 0;
    if (!pGetImageSize(g_utils, img, &w, &h) || !w || !h || w > 512 || h > 512) return false;
    out.w = (int)w;
    out.h = (int)h;
    out.rgba.resize((size_t)w * h * 4);
    return pGetImageRGBA(g_utils, img, out.rgba.data(), (int)out.rgba.size());
}

void Pump() {
    if (Init() && pRunCallbacks) pRunCallbacks();
}

}  // namespace steam
