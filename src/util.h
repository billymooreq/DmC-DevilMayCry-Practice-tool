#pragma once
#include <windows.h>
#include <cstdint>
#include <string>

namespace util {

// Directory of the host exe (Binaries\Win32 for the game), with trailing backslash.
const std::wstring& ExeDir();
std::wstring InExeDir(const wchar_t* name);

std::string WideToUtf8(const std::wstring& w);
std::wstring Utf8ToWide(const std::string& s);
std::string Hex(uintptr_t v);

// Memory reads that never crash: an access violation returns false.
bool SafeRead(uintptr_t addr, void* out, size_t size);

template <class T> bool Read(uintptr_t addr, T& out) { return SafeRead(addr, &out, sizeof(T)); }
template <class T> T ReadOr(uintptr_t addr, T def = T{}) {
  T v;
  return SafeRead(addr, &v, sizeof(T)) ? v : def;
}

// Plausible user-mode pointer for a 32-bit LAA process.
inline bool PlausibleAddr(uintptr_t p) { return p >= 0x10000 && p < 0xFFFE0000; }
// Same, plus 4-byte alignment (heap objects, arrays). FNameEntry pointers are NOT aligned in DmC.
inline bool PlausiblePtr(uintptr_t p) { return PlausibleAddr(p) && (p & 3) == 0; }

// Reads a NUL-terminated string page by page (never reads past the terminator's page).
std::string ReadCString(uintptr_t addr, size_t maxLen);
std::wstring ReadWString(uintptr_t addr, size_t maxLen);

}  // namespace util
