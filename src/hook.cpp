#include "hook.h"

#include <windows.h>

#include <cstring>

namespace hook {

void* PatchIAT(void* module, const char* dll, const char* func, void* repl) {
    auto base = (uint8_t*)module;
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; ++imp) {
        if (_stricmp((const char*)(base + imp->Name), dll) != 0) continue;
        auto names = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
        auto slots = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            auto ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
            if (strcmp((const char*)ibn->Name, func) != 0) continue;
            void* old = (void*)slots->u1.Function;
            WritePtr((void**)&slots->u1.Function, repl);
            return old;
        }
    }
    return nullptr;
}

void WritePtr(void** slot, void* value) {
    DWORD oldProt;
    VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProt);
    *slot = value;
    VirtualProtect(slot, sizeof(void*), oldProt, &oldProt);
}

static void WriteAbsJmp(uint8_t* at, void* dest) {
    // jmp qword ptr [rip+0]; dq dest
    at[0] = 0xFF;
    at[1] = 0x25;
    memset(at + 2, 0, 4);
    memcpy(at + 6, &dest, 8);
}

void* Detour(void* target, const uint8_t* expected, size_t len, void* repl) {
    if (len < 14) return nullptr;
    if (memcmp(target, expected, len) != 0) return nullptr;
    auto tramp = (uint8_t*)VirtualAlloc(nullptr, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return nullptr;
    memcpy(tramp, target, len);
    WriteAbsJmp(tramp + len, (uint8_t*)target + len);
    FlushInstructionCache(GetCurrentProcess(), tramp, 64);

    DWORD oldProt;
    VirtualProtect(target, len, PAGE_EXECUTE_READWRITE, &oldProt);
    WriteAbsJmp((uint8_t*)target, repl);
    for (size_t i = 14; i < len; ++i) ((uint8_t*)target)[i] = 0x90;  // nop tail
    VirtualProtect(target, len, oldProt, &oldProt);
    FlushInstructionCache(GetCurrentProcess(), target, len);
    return tramp;
}

}  // namespace hook
