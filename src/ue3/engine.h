#pragma once
#include <cstdint>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

// Calling UnrealScript functions. UFunction calls are only safe on the game thread, so callers post tasks
// into a queue that the ProcessEvent hook drains on the game thread (outermost ProcessEvent call only).
namespace engine {

bool InstallProcessEventHook();  // needs ue3::Init() for layouts; idempotent
bool Hooked();
uintptr_t ProcessEventAddr();
const char* ProcessEventSignature();  // byte pattern (compatibility report)

// Runs fn on the game thread at the next top-level ProcessEvent.
void Post(std::function<void()> fn);
size_t Pending();
void LogTimeout(int timeoutMs);

// Runs fn on the game thread and waits for its result (default value on timeout). Worker threads only.
template <class T>
T OnGame(std::function<T()> fn, int timeoutMs = 10000) {
  auto prom = std::make_shared<std::promise<T>>();
  std::future<T> fut = prom->get_future();
  Post([prom, fn] { prom->set_value(fn()); });
  if (fut.wait_for(std::chrono::milliseconds(timeoutMs)) != std::future_status::ready) {
    LogTimeout(timeoutMs);
    return T{};
  }
  return fut.get();
}
bool OnGameThread();
bool GameThreadKnown();

// UFunction lookup along the class chain of obj (cached).
uintptr_t FindFunction(uintptr_t obj, const char* name);

struct Param {
  std::string name, type;  // type = property class name ("IntProperty", ...)
  int offset, size;
  uint64_t flags;
  uint32_t boolMask;
  uintptr_t prop;
};
std::vector<Param> Params(uintptr_t func);  // CPF_Parm children, declaration order (return value included)

// Parameter buffer for one call. Values not set stay zero.
class Call {
 public:
  Call(uintptr_t obj, const char* funcName);
  // Explicit UFunction (e.g. a static function of another class, called with a live object as context:
  // ProcessEvent appears to reject class default objects).
  Call(uintptr_t obj, uintptr_t func);
  bool Valid() const { return fn_ != 0; }
  uintptr_t Function() const { return fn_; }
  Call& Int(const char* p, int v);
  Call& Float(const char* p, float v);
  Call& Bool(const char* p, bool v);
  Call& Byte(const char* p, uint8_t v);
  Call& Obj(const char* p, uintptr_t v);
  Call& Name(const char* p, const char* nameStr);  // name must already exist in GNames
  Call& Str(const char* p, const std::wstring& s); // FString -> buffer owned by this Call
  Call& Raw(const char* p, const void* data, int size);  // structs (e.g. StatId = FName)
  // Positional setters (non-return params in declaration order).
  const std::string& ParamName(int i) const;
  const uint8_t* ParamData(int i) const;  // value of a (non-return) parameter after Invoke: out parameters
  bool Invoke();  // game thread only; false if invalid / unknown param was set
  int RetInt() const;
  float RetFloat() const;
  uint8_t RetByte() const;
  bool RetBool() const;
  const uint8_t* RetData() const;  // the return value in the parameter buffer (FName, struct, object); null if none
  std::string Describe() const;  // "Class.Func(a=1, ...)"
  const std::string& Error() const { return err_; }
 private:
  const Param* Find(const char* p);
  uintptr_t obj_ = 0, fn_ = 0;
  std::vector<Param> params_;
  std::vector<uint8_t> buf_;
  std::vector<std::wstring> strings_;
  bool bad_ = false;
  std::string err_;
};

// Per-frame game-thread callback: the render thread counts frames (NotifyFrame from Present), the first top-level
// ProcessEvent on the game thread after a new frame runs the callback once.
void NotifyFrame();
unsigned FrameCount();  // presented frames so far (autorun fps measurement)
void SetFrameCallback(void (*fn)());

// Observer of every ProcessEvent call on the game thread (before the original runs; may edit parms).
// Must be cheap: compare fn against a few cached UFunction pointers.
using EventHook = void (*)(void* self, void* fn, void* parms);
void SetEventHook(EventHook h);
void SetEventHookAfter(EventHook h);  // same, right after the original returned (results of the call)

// Optional ProcessEvent log (debug.pe_log): lines with "Object.Function" containing any filter substring.
void SetPeLog(bool on, const std::string& commaSeparatedFilter);

}  // namespace engine
