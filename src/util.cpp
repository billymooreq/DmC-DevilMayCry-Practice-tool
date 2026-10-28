#include "util.h"

#include <cstring>

namespace util {

const std::wstring& ExeDir() {
  static const std::wstring dir = [] {
    wchar_t buf[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    std::wstring s = buf;
    size_t p = s.find_last_of(L"\\/");
    return p == std::wstring::npos ? std::wstring(L".\\") : s.substr(0, p + 1);
  }();
  return dir;
}

std::wstring InExeDir(const wchar_t* name) { return ExeDir() + name; }

std::string WideToUtf8(const std::wstring& w) {
  if (w.empty()) return {};
  int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s(n, '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
  return s;
}

std::wstring Utf8ToWide(const std::string& s) {
  if (s.empty()) return {};
  int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w(n, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
  return w;
}

std::string Hex(uintptr_t v) {
  char b[16];
  snprintf(b, sizeof b, "0x%08X", (unsigned)v);
  return b;
}

// Kept free of C++ objects so __try/__except is allowed.
static bool SafeCopy(void* dst, const void* src, size_t n) {
  __try {
    memcpy(dst, src, n);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

bool SafeRead(uintptr_t addr, void* out, size_t size) {
  if (addr < 0x10000) return false;
  return SafeCopy(out, reinterpret_cast<const void*>(addr), size);
}

template <class Ch>
static std::basic_string<Ch> ReadStr(uintptr_t addr, size_t maxLen) {
  std::basic_string<Ch> out;
  while (out.size() < maxLen) {
    // read up to the end of the current page in one go
    size_t toPage = 0x1000 - (addr & 0xFFF);
    size_t chars = toPage / sizeof(Ch);
    if (chars == 0) chars = 1;
    if (chars > maxLen - out.size()) chars = maxLen - out.size();
    Ch buf[0x1000 / sizeof(Ch)];
    if (!SafeRead(addr, buf, chars * sizeof(Ch))) break;
    for (size_t i = 0; i < chars; ++i) {
      if (buf[i] == 0) return out;
      out.push_back(buf[i]);
    }
    addr += chars * sizeof(Ch);
  }
  return out;
}

std::string ReadCString(uintptr_t addr, size_t maxLen) { return ReadStr<char>(addr, maxLen); }
std::wstring ReadWString(uintptr_t addr, size_t maxLen) { return ReadStr<wchar_t>(addr, maxLen); }

}  // namespace util
