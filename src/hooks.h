#pragma once

// Thin MinHook wrapper. Every hook is logged by name.
namespace hooks {
bool EnsureInit();
bool Create(void* target, void* detour, void** original, const char* name);
bool Enable(void* target, const char* name);
}  // namespace hooks
