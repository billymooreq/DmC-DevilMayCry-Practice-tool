#include "ue3/engine.h"

#include <windows.h>

#include <atomic>
#include <deque>
#include <map>
#include <memory>
#include <mutex>

#include "hooks.h"
#include "log.h"
#include "ue3/ue3.h"
#include "util.h"

using util::ReadOr;

namespace engine {
namespace {

// UObject::ProcessEvent(UFunction*, void* Parms, void* Result), __thiscall, ret 0Ch.
// latest_2025: VA 0x0047B7B0 / RVA 0x0007B7B0, unique in .text.
constexpr const char* kSigProcessEvent =
    "55 8B EC 6A FF 68 ?? ?? ?? ?? 64 A1 00 00 00 00 50 83 EC 58 A1 ?? ?? ?? ?? 33 C5 89 45 F0 53 56 57 50 "
    "8D 45 F4 64 A3 00 00 00 00 8B F1 89 75 EC 8B 5D 08 85 DB";

using PE_t = void(__thiscall*)(void* self, void* fn, void* parms, void* result);
PE_t g_origPE = nullptr;
uintptr_t g_peAddr = 0;
std::atomic<bool> g_hooked{false};
std::atomic<DWORD> g_gameThread{0};
thread_local int t_depth = 0;

std::mutex g_qMu;
std::deque<std::function<void()>> g_queue;
std::atomic<size_t> g_pending{0};

std::atomic<unsigned> g_frame{0};
thread_local unsigned t_lastFrame = 0;
std::atomic<void (*)()> g_frameCb{nullptr};
std::atomic<EventHook> g_eventHook{nullptr};
std::atomic<EventHook> g_eventHookAfter{nullptr};

std::atomic<bool> g_peLog{false};
std::mutex g_filterMu;  // guards the pointer swap only; the vector itself is immutable
std::shared_ptr<const std::vector<std::string>> g_peFilter = std::make_shared<std::vector<std::string>>();
std::atomic<int> g_peLines{0};
constexpr int kPeLogMax = 20000;

void Pump() {
  std::deque<std::function<void()>> run;
  {
    std::lock_guard<std::mutex> lk(g_qMu);
    run.swap(g_queue);
    g_pending = 0;
  }
  for (auto& f : run) f();
}

void LogEvent(void* self, void* fn) {
  if (!ue3::Ready() || g_peLines >= kPeLogMax) return;
  std::shared_ptr<const std::vector<std::string>> filter;
  {
    std::lock_guard<std::mutex> lk(g_filterMu);
    filter = g_peFilter;
  }
  std::string func = ue3::ObjName((uintptr_t)fn);
  std::string obj = ue3::ObjName((uintptr_t)self);
  std::string full = obj + "." + func;
  bool hit = filter->empty();
  for (const std::string& f : *filter)
    if (!f.empty() && full.find(f) != std::string::npos) hit = true;
  if (!hit) return;
  static thread_local std::string last;
  static thread_local int repeats = 0;
  if (full == last) {
    ++repeats;
    return;
  }
  if (repeats) LOG("PE   (x%d more)", repeats);
  last = full;
  repeats = 0;
  ++g_peLines;
  LOG("PE %s  [%s, func %s]", full.c_str(), ue3::ClassNameOf((uintptr_t)self).c_str(),
      ue3::PathName((uintptr_t)fn).c_str());
}

void __fastcall hkProcessEvent(void* self, void* /*edx*/, void* fn, void* parms, void* result) {
  DWORD tid = GetCurrentThreadId();
  DWORD expected = 0;
  if (g_gameThread.compare_exchange_strong(expected, tid)) LOG("engine: game thread is %lu", tid);
  if (t_depth == 0 && tid == g_gameThread) {
    if (g_pending) {
      ++t_depth;
      Pump();
      --t_depth;
    }
    unsigned fr = g_frame;
    if (fr != t_lastFrame) {
      t_lastFrame = fr;
      if (auto cb = g_frameCb.load()) {
        ++t_depth;
        cb();
        --t_depth;
      }
    }
  }
  if (g_peLog) LogEvent(self, fn);
  if (EventHook h = g_eventHook.load(); h && tid == g_gameThread) h(self, fn, parms);
  ++t_depth;
  g_origPE(self, fn, parms, result);
  --t_depth;
  if (EventHook h = g_eventHookAfter.load(); h && tid == g_gameThread) h(self, fn, parms);
}

std::mutex g_fnMu;
std::map<std::pair<uintptr_t, int>, uintptr_t> g_fnCache;

}  // namespace

bool InstallProcessEventHook() {
  if (g_hooked) return true;
  static std::mutex mu;
  std::lock_guard<std::mutex> lk(mu);
  if (g_hooked) return true;
  g_peAddr = ue3::SigScan(kSigProcessEvent);
  if (!g_peAddr) {
    LOG("engine: ProcessEvent signature not found - game calls disabled");
    return false;
  }
  LOG("engine: ProcessEvent at %p (RVA 0x%X)", (void*)g_peAddr,
      (unsigned)(g_peAddr - (uintptr_t)GetModuleHandleW(nullptr)));
  if (!hooks::Create((void*)g_peAddr, (void*)&hkProcessEvent, (void**)&g_origPE, "ProcessEvent") ||
      !hooks::Enable((void*)g_peAddr, "ProcessEvent"))
    return false;
  g_hooked = true;
  return true;
}

bool Hooked() { return g_hooked; }
void NotifyFrame() { ++g_frame; }
unsigned FrameCount() { return g_frame; }
void SetFrameCallback(void (*fn)()) { g_frameCb = fn; }
void SetEventHook(EventHook h) { g_eventHook = h; }
void SetEventHookAfter(EventHook h) { g_eventHookAfter = h; }
uintptr_t ProcessEventAddr() { return g_peAddr; }
const char* ProcessEventSignature() { return kSigProcessEvent; }

void Post(std::function<void()> fn) {
  std::lock_guard<std::mutex> lk(g_qMu);
  g_queue.push_back(std::move(fn));
  g_pending = g_queue.size();
}

size_t Pending() { return g_pending; }

void LogTimeout(int timeoutMs) { LOG("engine: game-thread task timed out (%d ms)", timeoutMs); }
bool GameThreadKnown() { return g_gameThread != 0; }
bool OnGameThread() { return g_gameThread != 0 && GetCurrentThreadId() == g_gameThread; }

void SetPeLog(bool on, const std::string& filter) {
  auto v = std::make_shared<std::vector<std::string>>();
  size_t start = 0;
  while (start <= filter.size()) {
    size_t comma = filter.find(',', start);
    std::string part = filter.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
    while (!part.empty() && part.front() == ' ') part.erase(part.begin());
    while (!part.empty() && part.back() == ' ') part.pop_back();
    if (!part.empty()) v->push_back(part);
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  {
    std::lock_guard<std::mutex> lk(g_filterMu);
    g_peFilter = v;
  }
  if (on != g_peLog) LOG("engine: ProcessEvent log %s (filter '%s')", on ? "ON" : "off", filter.c_str());
  g_peLog = on;
}

uintptr_t FindFunction(uintptr_t obj, const char* name) {
  int ni = ue3::FindNameIndex(name);
  uintptr_t cls = ue3::ObjClass(obj);
  if (ni < 0 || !cls) return 0;
  {
    std::lock_guard<std::mutex> lk(g_fnMu);
    auto it = g_fnCache.find({cls, ni});
    if (it != g_fnCache.end()) return it->second;
  }
  uintptr_t found = 0;
  for (uintptr_t s = cls; s && !found; s = ue3::StructSuper(s)) {
    int guard = 0;
    for (uintptr_t f = ue3::StructChildren(s); f && guard < 10000; f = ue3::FieldNext(f), ++guard) {
      if (ue3::ObjNameIndex(f) == ni && ue3::ClassNameOf(f) == "Function") {
        found = f;
        break;
      }
    }
  }
  std::lock_guard<std::mutex> lk(g_fnMu);
  g_fnCache[{cls, ni}] = found;
  return found;
}

std::vector<Param> Params(uintptr_t func) {
  std::vector<Param> all, flagged;
  int np = ue3::FuncNumParms(func);
  int guard = 0;
  for (uintptr_t c = ue3::StructChildren(func); c && guard < 256; c = ue3::FieldNext(c), ++guard) {
    std::string cls = ue3::ClassNameOf(c);
    if (cls.size() < 8 || cls.compare(cls.size() - 8, 8, "Property") != 0) continue;
    Param p{ue3::ObjName(c), cls, ue3::PropOffset(c), ue3::PropElementSize(c) * std::max(1, ue3::PropArrayDim(c)),
            ue3::PropFlags(c), cls == "BoolProperty" ? ue3::PropExtraU32(c) : 0u, c};
    all.push_back(p);
    if (p.flags & 0x80) flagged.push_back(p);
  }
  // CPF_Parm is trusted only if it agrees with NumParms; otherwise params are the first NumParms children.
  if (np >= 0 && (int)flagged.size() == np) return flagged;
  if (np >= 0 && np <= (int)all.size()) return std::vector<Param>(all.begin(), all.begin() + np);
  return flagged;
}

// ------------------------------------------------------------------ Call
Call::Call(uintptr_t obj, const char* funcName) : obj_(obj) {
  if (!ue3::IsObject(obj)) {
    err_ = "bad object";
    return;
  }
  fn_ = FindFunction(obj, funcName);
  if (!fn_) {
    err_ = std::string("no function ") + funcName;
    return;
  }
  params_ = Params(fn_);
  int size = std::max(ue3::FuncParmsSize(fn_), ue3::StructSize(fn_));
  for (const Param& p : params_) size = std::max(size, p.offset + p.size);
  buf_.assign(size + 16, 0);
}

Call::Call(uintptr_t obj, uintptr_t func) : obj_(obj) {
  if (!ue3::IsObject(obj) || !ue3::IsObject(func)) {
    err_ = "bad object/function";
    return;
  }
  fn_ = func;
  params_ = Params(fn_);
  int size = std::max(ue3::FuncParmsSize(fn_), ue3::StructSize(fn_));
  for (const Param& p : params_) size = std::max(size, p.offset + p.size);
  buf_.assign(size + 16, 0);
}

const std::string& Call::ParamName(int i) const {
  static const std::string none;
  int k = 0;
  for (const Param& p : params_) {
    if (p.flags & 0x400) continue;  // return value
    if (k++ == i) return p.name;
  }
  return none;
}

const uint8_t* Call::ParamData(int i) const {
  int k = 0;
  for (const Param& p : params_) {
    if (p.flags & 0x400) continue;
    if (k++ == i) return &buf_[p.offset];
  }
  return nullptr;
}

Call& Call::Raw(const char* p, const void* data, int size) {
  if (auto x = Find(p)) memcpy(&buf_[x->offset], data, std::min(size, x->size));
  return *this;
}

const Param* Call::Find(const char* p) {
  for (const Param& x : params_)
    if (x.name == p) return &x;
  bad_ = true;
  err_ = std::string("no param ") + p;
  return nullptr;
}

Call& Call::Int(const char* p, int v) {
  if (auto x = Find(p)) memcpy(&buf_[x->offset], &v, 4);
  return *this;
}
Call& Call::Float(const char* p, float v) {
  if (auto x = Find(p)) memcpy(&buf_[x->offset], &v, 4);
  return *this;
}
Call& Call::Byte(const char* p, uint8_t v) {
  if (auto x = Find(p)) buf_[x->offset] = v;
  return *this;
}
Call& Call::Bool(const char* p, bool v) {
  if (auto x = Find(p)) {
    uint32_t d;
    memcpy(&d, &buf_[x->offset], 4);
    uint32_t m = x->boolMask ? x->boolMask : 1;
    d = v ? (d | m) : (d & ~m);
    memcpy(&buf_[x->offset], &d, 4);
  }
  return *this;
}
Call& Call::Obj(const char* p, uintptr_t v) {
  if (auto x = Find(p)) memcpy(&buf_[x->offset], &v, 4);
  return *this;
}
Call& Call::Name(const char* p, const char* nameStr) {
  if (auto x = Find(p)) {
    int idx = ue3::FindNameIndex(nameStr);
    if (idx < 0) {
      bad_ = true;
      err_ = std::string("name not in GNames: ") + nameStr;
      return *this;
    }
    ue3::FNameRaw n{idx, 0};
    memcpy(&buf_[x->offset], &n, 8);
  }
  return *this;
}
Call& Call::Str(const char* p, const std::wstring& s) {
  if (auto x = Find(p)) {
    strings_.push_back(s);
    std::wstring& w = strings_.back();
    ue3::TArrayRaw a{(uintptr_t)w.c_str(), (int32_t)w.size() + 1, (int32_t)w.size() + 1};
    memcpy(&buf_[x->offset], &a, sizeof a);
  }
  return *this;
}

bool Call::Invoke() {
  if (!fn_ || bad_) {
    static std::mutex mu;
    static std::map<uintptr_t, int> logged;  // a per-frame caller must not flood the log
    std::lock_guard<std::mutex> lk(mu);
    if (++logged[fn_] <= 3) LOG("call %s: not invoked (%s)", Describe().c_str(), err_.c_str());
    return false;
  }
  if (!OnGameThread() || !g_origPE) {
    LOG("call %s: refused - not on the game thread", Describe().c_str());
    return false;
  }
  ++t_depth;
  g_origPE((void*)obj_, (void*)fn_, buf_.data(), nullptr);
  --t_depth;
  return true;
}

int Call::RetInt() const {
  for (const Param& p : params_)
    if (p.flags & 0x400) {
      int v;
      memcpy(&v, &buf_[p.offset], 4);
      return v;
    }
  if (!params_.empty()) {  // fallback: last param is the return value in UE3 declaration order
    int v;
    memcpy(&v, &buf_[params_.back().offset], 4);
    return v;
  }
  return 0;
}

float Call::RetFloat() const {
  int v = RetInt();
  float f;
  memcpy(&f, &v, 4);
  return f;
}

uint8_t Call::RetByte() const { return (uint8_t)(RetInt() & 0xFF); }

bool Call::RetBool() const {
  for (const Param& p : params_)
    if (p.flags & 0x400) {
      uint32_t d;
      memcpy(&d, &buf_[p.offset], 4);
      return (d & (p.boolMask ? p.boolMask : 1)) != 0;
    }
  return RetInt() != 0;
}

const uint8_t* Call::RetData() const {
  for (const Param& p : params_)
    if (p.flags & 0x400) return &buf_[p.offset];
  return params_.empty() ? nullptr : &buf_[params_.back().offset];
}

std::string Call::Describe() const {
  std::string s = (obj_ ? ue3::ObjName(obj_) : std::string("?")) + "." + (fn_ ? ue3::ObjName(fn_) : "?") + "(";
  for (size_t i = 0; i < params_.size(); ++i) s += (i ? ", " : "") + params_[i].type + " " + params_[i].name;
  return s + ")";
}

}  // namespace engine
