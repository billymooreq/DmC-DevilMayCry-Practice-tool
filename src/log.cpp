#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>

#include "util.h"

namespace logx {

static std::mutex g_mu;
static FILE* g_file = nullptr;
static bool g_opened = false;
static std::deque<std::string> g_tail;

void Write(const char* fmt, ...) {
  char msg[2048];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof msg, fmt, ap);
  va_end(ap);

  SYSTEMTIME st;
  GetLocalTime(&st);
  char line[2200];
  snprintf(line, sizeof line, "[%02d:%02d:%02d.%03d][%5lu] %s", st.wHour, st.wMinute, st.wSecond,
           st.wMilliseconds, GetCurrentThreadId(), msg);

  std::lock_guard<std::mutex> lk(g_mu);
  if (!g_opened) {
    g_opened = true;
    g_file = _wfopen(util::InExeDir(L"DmCPracticeTool.log").c_str(), L"w");
  }
  if (g_file) {
    fputs(line, g_file);
    fputc('\n', g_file);
    fflush(g_file);
  }
  g_tail.emplace_back(line);
  if (g_tail.size() > 300) g_tail.pop_front();
}

std::vector<std::string> Tail(size_t maxLines) {
  std::lock_guard<std::mutex> lk(g_mu);
  size_t n = g_tail.size() < maxLines ? g_tail.size() : maxLines;
  return std::vector<std::string>(g_tail.end() - n, g_tail.end());
}

}  // namespace logx
