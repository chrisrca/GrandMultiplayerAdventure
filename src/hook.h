#pragma once
#include <cstddef>
#include <cstdint>

namespace hook {

// Replace the import-address-table slot for dll!func in `module` with `repl`.
// Returns the previous function pointer, or nullptr if not found.
void* PatchIAT(void* module, const char* dll, const char* func, void* repl);

// Absolute-jump detour at `target`. `expected` must match the first `len`
// (>= 14) bytes, which must be position independent; they are relocated
// verbatim into a trampoline. Returns the trampoline (call it to run the
// original function) or nullptr on mismatch.
void* Detour(void* target, const uint8_t* expected, size_t len, void* repl);

// Overwrite a pointer-sized variable (e.g. a glew function pointer).
void WritePtr(void** slot, void* value);

}  // namespace hook
