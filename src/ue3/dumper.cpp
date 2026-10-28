#include "ue3/dumper.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_set>
#include <vector>

#include "config.h"
#include "log.h"
#include "ue3/ue3.h"
#include "util.h"
#include "version.h"

using namespace ue3;
using util::ReadOr;

namespace dumper {
namespace {

std::atomic<bool> g_busy{false};
std::string g_arg;  // custom filter of the running dump (worker thread only)

std::vector<std::string> SplitList(const std::string& s, char sep) {
  std::vector<std::string> out;
  size_t start = 0;
  while (start <= s.size()) {
    size_t e = s.find(sep, start);
    std::string part = s.substr(start, e == std::string::npos ? std::string::npos : e - start);
    while (!part.empty() && part.front() == ' ') part.erase(part.begin());
    while (!part.empty() && part.back() == ' ') part.pop_back();
    if (!part.empty()) out.push_back(part);
    if (e == std::string::npos) break;
    start = e + 1;
  }
  return out;
}
std::mutex g_resMu;
std::string g_result = "no dumps yet";

void SetResult(const std::string& s) {
  std::lock_guard<std::mutex> lk(g_resMu);
  g_result = s;
  LOG("dump: %s", s.c_str());
}

std::wstring DumpDir() {
  std::wstring d = util::InExeDir(L"DmCPT_dumps\\");
  CreateDirectoryW(d.c_str(), nullptr);
  return d;
}

struct Out {
  FILE* f = nullptr;
  std::wstring path;
  explicit Out(const std::wstring& name) : path(DumpDir() + name) {
    f = _wfopen(path.c_str(), L"w");
    if (f) setvbuf(f, nullptr, _IOFBF, 1 << 20);
  }
  ~Out() {
    if (f) fclose(f);
  }
  void Line(const char* fmt, ...) {
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
  }
  std::string Name() const { return util::WideToUtf8(path.substr(path.find_last_of(L'\\') + 1)); }
};

std::string Timestamp() {
  SYSTEMTIME st;
  GetLocalTime(&st);
  char b[32];
  snprintf(b, sizeof b, "%04d%02d%02d_%02d%02d%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
           st.wSecond);
  return b;
}

void Header(Out& o, const char* what) {
  const VersionInfo& v = GetGameVersion();
  o.Line("# DmC Practice Tool dump: %s", what);
  o.Line("# game build %s, TimeDateStamp 0x%08X, base 0x%08X, %s", v.name, v.timeDateStamp, (unsigned)v.base,
         Timestamp().c_str());
}

bool EndsWith(const std::string& s, const char* suf) {
  size_t n = strlen(suf);
  return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
}
bool StartsWith(const std::string& s, const char* pre) { return s.rfind(pre, 0) == 0; }

bool IsProperty(uintptr_t f) { return EndsWith(ClassNameOf(f), "Property"); }

// ------------------------------------------------------------------- flags
std::string PropFlagsText(uint64_t f) {
  static const std::pair<uint64_t, const char*> k[] = {
      {0x1, "Edit"},           {0x2, "Const"},          {0x10, "OptionalParm"},  {0x20, "Net"},
      {0x80, "Parm"},          {0x100, "OutParm"},      {0x400, "ReturnParm"},   {0x1000, "Native"},
      {0x2000, "Transient"},   {0x4000, "Config"},      {0x8000, "Localized"},   {0x20000, "EditConst"},
      {0x40000, "GlobalConfig"}, {0x80000, "Component"}, {0x200000, "DuplicateTransient"},
      {0x800000, "NoExport"},  {0x4000000, "EditInline"}, {0x20000000, "Deprecated"},
      {0x100000000ull, "RepNotify"}, {0x200000000ull, "Interp"}, {0x800000000ull, "EditorOnly"}};
  std::string s;
  for (auto& [bit, name] : k)
    if (f & bit) s += s.empty() ? name : std::string("|") + name;
  return s;
}

std::string FuncFlagsText(uint32_t f) {
  static const std::pair<uint32_t, const char*> k[] = {
      {0x1, "final"},       {0x2, "defined"},    {0x4, "iterator"},  {0x8, "latent"},
      {0x40, "net"},        {0x100, "simulated"}, {0x200, "EXEC"},    {0x400, "NATIVE"},
      {0x800, "event"},     {0x1000, "operator"}, {0x2000, "static"}, {0x8000, "const"},
      {0x20000, "public"},  {0x40000, "private"}, {0x80000, "protected"}, {0x100000, "delegate"},
      {0x200000, "server"}, {0x400000, "hasOut"}, {0x800000, "hasDefaults"}, {0x1000000, "client"}};
  std::string s;
  for (auto& [bit, name] : k)
    if (f & bit) s += s.empty() ? name : std::string(" ") + name;
  return s;
}

// ------------------------------------------------------------------- types
std::string TypeName(uintptr_t p, int depth = 0) {
  std::string c = ClassNameOf(p);
  if (depth > 4) return c;
  uintptr_t x = PropExtra(p);
  auto nameOrQ = [](uintptr_t o) { return IsObject(o) ? ObjName(o) : std::string("?"); };
  if (c == "ByteProperty") return IsObject(x) ? "enum " + ObjName(x) : "byte";
  if (c == "IntProperty") return "int";
  if (c == "FloatProperty") return "float";
  if (c == "BoolProperty") return "bool";
  if (c == "NameProperty") return "FName";
  if (c == "StrProperty") return "FString";
  if (c == "ObjectProperty" || c == "ComponentProperty") return nameOrQ(x) + "*";
  if (c == "ClassProperty") return "class<" + nameOrQ(PropExtra(p, 1)) + ">";
  if (c == "StructProperty") return "struct " + nameOrQ(x);
  if (c == "ArrayProperty") return "TArray<" + (IsObject(x) ? TypeName(x, depth + 1) : "?") + ">";
  if (c == "InterfaceProperty") return "interface " + nameOrQ(x);
  if (c == "DelegateProperty") return "delegate " + nameOrQ(x);
  if (c == "MapProperty") return "map";
  return c;
}

template <class Fn>
void ForEachField(uintptr_t st, Fn fn) {
  int guard = 0;
  for (uintptr_t f = StructChildren(st); f && guard < 10000; f = FieldNext(f), ++guard) fn(f);
}

std::vector<uintptr_t> ChainRootFirst(uintptr_t st) {
  std::vector<uintptr_t> chain;
  for (int d = 0; st && d < 64; ++d, st = StructSuper(st)) chain.push_back(st);
  return std::vector<uintptr_t>(chain.rbegin(), chain.rend());
}

std::string FuncSignature(uintptr_t fn) {
  std::string ret = "void", params;
  ForEachField(fn, [&](uintptr_t p) {
    if (!IsProperty(p)) return;
    uint64_t fl = PropFlags(p);
    if (!(fl & 0x80)) return;  // locals
    if (fl & 0x400) {
      ret = TypeName(p);
      return;
    }
    if (!params.empty()) params += ", ";
    if (fl & 0x100) params += "out ";
    if (fl & 0x10) params += "optional ";
    params += TypeName(p) + " " + ObjName(p);
  });
  return ret + " " + ObjName(fn) + "(" + params + ")";
}

// ------------------------------------------------------------------- names / objects
void DumpNames() {
  Out o(L"names.txt");
  Header(o, "names");
  int n = NumNames();
  for (int i = 0; i < n; ++i) o.Line("[%6d] %s", i, Name(i).c_str());
  SetResult(o.Name() + ": " + std::to_string(n) + " names");
}

void DumpObjects() {
  Out o(L"objects.txt");
  Header(o, "objects");
  int n = NumObjects(), live = 0;
  for (int i = 0; i < n; ++i) {
    uintptr_t obj = Object(i);
    if (!obj) continue;
    ++live;
    o.Line("[%6d] 0x%08X %s %s", i, (unsigned)obj, ClassNameOf(obj).c_str(), PathName(obj).c_str());
  }
  SetResult(o.Name() + ": " + std::to_string(live) + " objects");
}

// ------------------------------------------------------------------- classes
bool WantedClassName(const std::string& n) {
  static const std::set<std::string> extra = {"CheatManager", "PlayerController", "GameInfo", "WorldInfo",
                                              "Object", "Actor", "GameEngine", "Engine"};
  return StartsWith(n, "DV") || StartsWith(n, "NT") || extra.count(n) || n.find("Cheat") != std::string::npos;
}

void DumpStructDecl(Out& o, uintptr_t st, bool isClass) {
  uintptr_t sup = StructSuper(st);
  o.Line("");
  o.Line("=== %s %s : %s   [%s] obj=0x%08X size=0x%X", isClass ? "class" : "struct", ObjName(st).c_str(),
         sup ? ObjName(sup).c_str() : "-", PathName(st).c_str(), (unsigned)st, StructSize(st));
  if (isClass) {
    uintptr_t cdo = ClassDefaultObject(st);
    if (cdo) o.Line("    CDO %s @0x%08X", ObjName(cdo).c_str(), (unsigned)cdo);
  }
  std::vector<uintptr_t> funcs, others;
  ForEachField(st, [&](uintptr_t f) {
    std::string c = ClassNameOf(f);
    if (EndsWith(c, "Property")) {
      uint64_t fl = PropFlags(f);
      std::string extra;
      if (c == "BoolProperty") {
        char b[24];
        snprintf(b, sizeof b, " mask=0x%X", PropExtraU32(f));
        extra = b;
      }
      o.Line("    +0x%04X [0x%04X x%d] %-36s %-32s flags=0x%llX %s%s", PropOffset(f), PropElementSize(f),
             PropArrayDim(f), TypeName(f).c_str(), ObjName(f).c_str(), (unsigned long long)fl,
             PropFlagsText(fl).c_str(), extra.c_str());
    } else if (c == "Function") {
      funcs.push_back(f);
    } else {
      others.push_back(f);
    }
  });
  for (uintptr_t f : others) {
    std::string c = ClassNameOf(f);
    if (c == "Enum") {
      std::string vals;
      for (auto& v : EnumNames(f)) vals += (vals.empty() ? "" : ", ") + v;
      o.Line("    enum %s { %s }", ObjName(f).c_str(), vals.c_str());
    } else {
      o.Line("    %s %s", c.c_str(), ObjName(f).c_str());
    }
  }
  for (uintptr_t f : funcs) {
    uint32_t fl = FuncFlags(f);
    o.Line("    func 0x%08X [%s] iNative=%u parms=%d/%dB  %s", fl, FuncFlagsText(fl).c_str(), FuncNative(f),
           FuncNumParms(f), FuncParmsSize(f), FuncSignature(f).c_str());
  }
}

// Custom list (autorun "dump_classes Class1,Class2"): only those classes -> classes_custom.txt.
void DumpClasses() {
  std::vector<std::string> custom = SplitList(g_arg, ',');
  std::set<std::string> customSet(custom.begin(), custom.end());
  auto wanted = [&](const std::string& n) { return custom.empty() ? WantedClassName(n) : customSet.count(n) > 0; };
  Out o(custom.empty() ? L"classes.txt" : L"classes_custom.txt");
  Header(o, custom.empty() ? "classes (DV*, NT*, *Cheat*, CheatManager, PlayerController, GameInfo, WorldInfo, Actor, Object)"
                           : ("classes: " + g_arg).c_str());
  o.Line("%s", LayoutReport().c_str());
  int n = NumObjects();
  std::unordered_set<uintptr_t> classes;
  std::vector<uintptr_t> classList, structList;
  for (int i = 0; i < n; ++i) {
    uintptr_t obj = Object(i);
    if (!obj) continue;
    std::string c = ClassNameOf(obj);
    if (c == "Class" && wanted(ObjName(obj))) {
      classes.insert(obj);
      classList.push_back(obj);
    }
  }
  for (int i = 0; i < n; ++i) {
    uintptr_t obj = Object(i);
    if (!obj || ClassNameOf(obj) != "ScriptStruct") continue;
    uintptr_t outer = ObjOuter(obj);
    if (classes.count(outer) || classes.count(ObjOuter(outer)) || (custom.empty() && WantedClassName(ObjName(obj))))
      structList.push_back(obj);
  }
  for (uintptr_t c : classList) DumpStructDecl(o, c, true);
  o.Line("");
  o.Line("##################### structs");
  for (uintptr_t s : structList) DumpStructDecl(o, s, false);
  SetResult(o.Name() + ": " + std::to_string(classList.size()) + " classes, " + std::to_string(structList.size()) +
            " structs");
}

// ------------------------------------------------------------------- live values
struct ValueDumper {
  Out& o;
  int arrayLimit;
  uintptr_t root = 0;
  std::deque<std::pair<uintptr_t, int>>* queue;
  std::unordered_set<uintptr_t>* seen;
  int depthOfRoot = 0;
  bool allBases = false;  // custom live dumps: engine bases too (Actor.Location, Physics, CustomTimeDilation...)

  std::string ObjRef(uintptr_t v) {
    if (!v) return "null";
    if (!IsObject(v)) return "<bad ptr " + util::Hex(v) + ">";
    // follow sub-objects of the dumped object and DV*/NT* non-actors (profile parts)
    std::string cn = ClassNameOf(v);
    if (queue && !seen->count(v) && depthOfRoot < 2 &&
        (ObjOuter(v) == root || ((StartsWith(cn, "DV") || StartsWith(cn, "NT")) && !IsA(v, "Actor")))) {
      seen->insert(v);
      queue->push_back({v, depthOfRoot + 1});
    }
    return FullName(v);
  }

  // Scalar types -> text; empty = not a scalar.
  std::string Scalar(uintptr_t p, const std::string& c, uintptr_t a) {
    char b[96];
    if (c == "IntProperty") {
      int v = ReadOr<int32_t>(a);
      snprintf(b, sizeof b, "%d (0x%X)", v, (unsigned)v);
      return b;
    }
    if (c == "FloatProperty") {
      snprintf(b, sizeof b, "%g", ReadOr<float>(a));
      return b;
    }
    if (c == "BoolProperty") {
      uint32_t mask = PropExtraU32(p);
      return (ReadOr<uint32_t>(a) & (mask ? mask : 1)) ? "true" : "false";
    }
    if (c == "ByteProperty") {
      uint8_t v = ReadOr<uint8_t>(a);
      uintptr_t e = PropExtra(p);
      std::string s = std::to_string(v);
      if (IsObject(e)) {
        auto names = EnumNames(e);
        s += " (" + (v < names.size() ? names[v] : std::string("?")) + ")";
      }
      return s;
    }
    if (c == "NameProperty") return "'" + ReadFName(a) + "'";
    if (c == "StrProperty") {
      TArrayRaw s{};
      util::Read(a, s);
      if (!s.data || s.num <= 0) return "\"\"";
      return "\"" + util::WideToUtf8(util::ReadWString(s.data, std::min(s.num, 512))) + "\"";
    }
    if (c == "ObjectProperty" || c == "ComponentProperty" || c == "ClassProperty")
      return ObjRef(ReadOr<uintptr_t>(a));
    if (c == "InterfaceProperty") return ObjRef(ReadOr<uintptr_t>(a));
    if (c == "DelegateProperty") {
      uintptr_t obj = ReadOr<uintptr_t>(a);
      return "delegate " + (obj ? (IsObject(obj) ? PathName(obj) : util::Hex(obj)) : "null") + "." +
             ReadFName(a + 4);
    }
    if (c == "MapProperty") return "<map, not dumped>";
    return {};
  }

  void Prop(uintptr_t p, uintptr_t base, int indent, int depth) {
    std::string c = ClassNameOf(p);
    std::string name = ObjName(p);
    int dim = std::max(1, std::min(PropArrayDim(p), 512));
    int esz = PropElementSize(p);
    int off = PropOffset(p);
    for (int i = 0; i < dim; ++i) {
      uintptr_t a = base + off + i * esz;
      std::string label = dim > 1 ? name + "[" + std::to_string(i) + "]" : name;
      Element(p, c, a, label, indent, depth, off + i * esz);
    }
  }

  void Element(uintptr_t p, const std::string& c, uintptr_t a, const std::string& label, int indent, int depth,
               int relOff) {
    std::string pad(indent * 2, ' ');
    char offs[16] = "";
    if (relOff >= 0) snprintf(offs, sizeof offs, "+0x%04X ", relOff);
    std::string sc = Scalar(p, c, a);
    if (!sc.empty()) {
      o.Line("%s%s%s = %s", pad.c_str(), offs, label.c_str(), sc.c_str());
      return;
    }
    if (c == "StructProperty") {
      uintptr_t st = PropExtra(p);
      o.Line("%s%s%s = struct %s {", pad.c_str(), offs, label.c_str(), IsObject(st) ? ObjName(st).c_str() : "?");
      if (IsObject(st) && depth < 8) StructValues(st, a, indent + 1, depth + 1);
      o.Line("%s}", pad.c_str());
      return;
    }
    if (c == "ArrayProperty") {
      TArrayRaw arr{};
      util::Read(a, arr);
      uintptr_t inner = PropExtra(p);
      o.Line("%s%s%s = %s num=%d max=%d data=0x%08X", pad.c_str(), offs, label.c_str(), TypeName(p).c_str(),
             arr.num, arr.max, (unsigned)arr.data);
      if (!IsObject(inner) || arr.num <= 0 || arr.num > 1000000 || !arr.data || depth >= 8) return;
      std::string ic = ClassNameOf(inner);
      int isz = PropElementSize(inner);
      int n = std::min(arr.num, arrayLimit);
      for (int i = 0; i < n; ++i)
        Element(inner, ic, arr.data + i * isz, "[" + std::to_string(i) + "]", indent + 1, depth + 1, -1);
      if (n < arr.num) o.Line("%s  ... %d more", pad.c_str(), arr.num - n);
      return;
    }
    o.Line("%s%s%s = <%s, %d bytes>", pad.c_str(), offs, label.c_str(), c.c_str(), PropElementSize(p));
  }

  void StructValues(uintptr_t st, uintptr_t base, int indent, int depth) {
    for (uintptr_t s : ChainRootFirst(st))
      ForEachField(s, [&](uintptr_t f) {
        if (IsProperty(f)) Prop(f, base, indent, depth);
      });
  }

  void HexDump(uintptr_t a, int size) {
    o.Line("  raw memory (%d bytes):", size);
    std::vector<uint8_t> buf(size);
    if (!util::SafeRead(a, buf.data(), size)) {
      o.Line("  <unreadable>");
      return;
    }
    for (int i = 0; i < size; i += 16) {
      char line[128];
      int k = snprintf(line, sizeof line, "  %04X:", i);
      for (int j = i; j < i + 16 && j < size; ++j) k += snprintf(line + k, sizeof line - k, " %02X", buf[j]);
      o.Line("%s", line);
    }
  }

  void DumpObj(uintptr_t obj, int depth) {
    root = obj;
    depthOfRoot = depth;
    uintptr_t cls = ObjClass(obj);
    o.Line("");
    o.Line("################ %s @0x%08X (depth %d)", FullName(obj).c_str(), (unsigned)obj, depth);
    std::string chain;
    for (uintptr_t s : ChainRootFirst(cls)) chain += (chain.empty() ? "" : " > ") + ObjName(s);
    o.Line("  class chain: %s, size 0x%X", chain.c_str(), StructSize(cls));
    bool actor = IsA(obj, "Actor");
    for (uintptr_t s : ChainRootFirst(cls)) {
      std::string sn = ObjName(s);
      // For actors only game classes (engine Actor/Controller bases are huge); for plain objects every
      // base except Object (profile data may live in an engine base like OnlineProfileSettings).
      if (s != cls && sn == "Object") continue;
      if (s != cls && actor && !allBases && !StartsWith(sn, "DV") && !StartsWith(sn, "NT")) continue;
      o.Line("  -- %s --", sn.c_str());
      ForEachField(s, [&](uintptr_t f) {
        if (IsProperty(f)) Prop(f, obj, 2, 0);
      });
    }
    int sz = StructSize(cls);
    if (sz > 0 && sz <= 0x10000) HexDump(obj, sz);
  }
};

void DumpProfile() {
  std::vector<std::string> custom = SplitList(g_arg, ',');
  static const char* kTargets[] = {"DVGamerProfile",   "NTGamerProfile",        "DVSaveManager",
                                   "NTSaveData",       "DVUpgradeSystem",       "DVSecretMissionManager",
                                   "DVStatsSystem",    "DVItemAcquireSystem",   "DVShopComponent",
                                   "DVPickupSystem",   "DVPlayerController_Player", "DVCheatManager"};
  // live_<time>_<first class>.txt: several dumps in the same second must not overwrite each other
  Out o(util::Utf8ToWide((custom.empty() ? "profile_" + Timestamp() : "live_" + Timestamp() + "_" + custom[0]) + ".txt"));
  Header(o, custom.empty() ? "live profile / progress objects" : ("live objects: " + g_arg).c_str());
  std::deque<std::pair<uintptr_t, int>> queue;
  std::unordered_set<uintptr_t> seen;
  // Compare FName indices, not strings: this walks every object's class chain.
  std::unordered_set<int> targetIdx;
  if (custom.empty())
    for (const char* t : kTargets)
      if (int i = FindNameIndex(t); i >= 0) targetIdx.insert(i);
  for (const std::string& t : custom)
    if (int i = FindNameIndex(t.c_str()); i >= 0) targetIdx.insert(i);
  const int classIdx = FindNameIndex("Class");
  int n = NumObjects();
  std::vector<std::string> found;
  for (int i = 0; i < n; ++i) {
    uintptr_t obj = Object(i);
    if (!obj) continue;
    uintptr_t cls = ObjClass(obj);
    if (!util::PlausiblePtr(cls) || ObjNameIndex(cls) == classIdx) continue;
    bool match = false;
    uintptr_t s = cls;
    for (int d = 0; s && !match && d < 64; ++d, s = StructSuper(s)) match = targetIdx.count(ObjNameIndex(s)) != 0;
    if (match && !custom.empty() && ObjName(obj).rfind("Default__", 0) == 0) match = false;
    if (match && !seen.count(obj)) {
      seen.insert(obj);
      queue.push_back({obj, 0});
      found.push_back(FullName(obj) + " @" + util::Hex(obj));
    }
  }
  o.Line("# targets found: %zu", found.size());
  for (auto& s : found) o.Line("#   %s", s.c_str());
  ValueDumper vd{o, std::max(1, g_cfg.dumpArrayLimit)};
  vd.queue = &queue;
  vd.seen = &seen;
  vd.allBases = !custom.empty();
  int dumped = 0;
  while (!queue.empty() && dumped < 80) {
    auto [obj, depth] = queue.front();
    queue.pop_front();
    vd.DumpObj(obj, depth);
    ++dumped;
  }
  SetResult(o.Name() + ": " + std::to_string(found.size()) + " targets, " + std::to_string(dumped) +
            " objects dumped");
}

// ------------------------------------------------------------------- script bytecode
// Heuristic reference scan of UnrealScript bytecode: in-memory script stores UObject* (functions,
// properties, classes) as raw 4-byte pointers, virtual calls as FName (8 bytes) and EX_StringConst (0x1F)
// as ANSI text. Enough to see what a function touches; full hex follows for manual decoding.
struct ScriptScan {
  std::unordered_set<int> funcNames;  // FName indices of every UFunction (virtual calls)
  std::vector<uintptr_t> calledFuncs;
};

std::vector<std::string> ScriptRefs(const std::vector<uint8_t>& code, ScriptScan& sc) {
  std::vector<std::string> refs;
  int nn = NumNames();
  size_t n = code.size();
  char b[64];
  for (size_t i = 0; i < n;) {
    if (i + 4 <= n) {
      uint32_t v;
      memcpy(&v, &code[i], 4);
      if (v >= 0x10000 && IsObject(v)) {
        snprintf(b, sizeof b, "  @%04X obj  ", (unsigned)i);
        refs.push_back(b + FullName(v));
        if (ClassNameOf(v) == "Function") sc.calledFuncs.push_back(v);
        i += 4;
        continue;
      }
    }
    if (i + 8 <= n) {
      FNameRaw nm;
      memcpy(&nm, &code[i], 8);
      // function names (virtual calls) anywhere; any valid name right after NameConst/VirtualFunction/
      // GlobalFunction/InstanceDelegate tokens (stat ids, upgrade names...)
      uint8_t prev = i ? code[i - 1] : 0xFF;
      bool nameTok = prev == 0x21 || prev == 0x1B || prev == 0x37 || prev == 0x4B;
      if (nm.index > 0 && nm.index < nn && nm.number >= 0 && nm.number < 1000 &&
          (sc.funcNames.count(nm.index) ? nm.number == 0 : nameTok)) {
        snprintf(b, sizeof b, "  @%04X name ", (unsigned)i);
        refs.push_back(b + NameOf(nm));
        i += 8;
        continue;
      }
    }
    if (code[i] == 0x1F) {
      size_t j = i + 1;
      while (j < n && code[j] >= 0x20 && code[j] < 0x7F) ++j;
      if (j < n && code[j] == 0 && j - i - 1 >= 2) {
        snprintf(b, sizeof b, "  @%04X str  \"", (unsigned)i);
        refs.push_back(b + std::string((const char*)&code[i + 1], j - i - 1) + "\"");
        i = j + 1;
        continue;
      }
    }
    ++i;
  }
  return refs;
}

bool ScriptKey(const std::string& fn) {
  static const char* keys[] = {"Unlock", "Save", "Give", "Costume", "Skin", "DLC", "Upgrade", "Health",
                               "DevilTrigger", "Difficult", "Complete", "Achievement", "Key", "Perk", "Tag",
                               "Award", "Level", "Profile", "QA", "Consumable", "Character", "Stat", "Mission"};
  for (const char* k : keys)
    if (fn.find(k) != std::string::npos) return true;
  return false;
}

// Custom filter (autorun "dump_scripts <classes>;<keywords>"): every function of the listed classes plus DV*/NT*
// functions whose name contains a keyword. Default: progress/unlock/save functions.
void DumpScripts() {
  std::vector<std::string> parts = SplitList(g_arg, ';');
  std::vector<std::string> customClasses = parts.size() > 0 ? SplitList(parts[0], ',') : std::vector<std::string>{};
  std::vector<std::string> customKeys = parts.size() > 1 ? SplitList(parts[1], ',') : std::vector<std::string>{};
  bool custom = !g_arg.empty();
  Out o(custom ? L"scripts_custom.txt" : L"scripts.txt");
  Header(o, custom ? ("UnrealScript bytecode: " + g_arg).c_str()
                   : "UnrealScript bytecode of progress/unlock/save related functions");
  o.Line("# UStruct.Script offset +0x%X; refs are heuristic (obj = UObject* operand, name = FName of a function,"
         " str = string constant)", L().structScript);
  static const std::set<std::string> whole = {"DVGamerProfile",   "NTGamerProfile",      "DVExecFunctions",
                                              "NTExecFunctions",  "NTCombat_UpgradesComponent", "DVSaveManager",
                                              "DVPickupStats",    "DVUpgradeSystem",     "DVItemAcquireSystem",
                                              "DVStats",          "NTDownloadableContentManager",
                                              "NTWorldInfo",      "DVGFxContainer_MissionSelectScreen",
                                              "DVGFxContainer_MissionDifficultySelect", "DVGFxContainer_ExtrasScreen",
                                              "DVGFxContainer_FrontendMenu", "DVGFxMovieHandler_Frontend_Callbacks",
                                              "DVGFxContainer_UpgradeScreen", "DVShopComponent", "DVUnlocksParams",
                                              "DVMapInfo",        "DVGameInfo",          "DVCombat_UpgradesComponent_Dante",
                                              "NTGameEngine",     "DVGameEngine",        "DVSeqCond_IsWearingCostume"};
  ScriptScan sc;
  std::vector<std::pair<uintptr_t, int>> queue;
  std::unordered_set<uintptr_t> seen;
  std::vector<std::pair<int, uintptr_t>> natives;  // iNative -> function, for decoding native tokens
  int n = NumObjects();
  for (int i = 0; i < n; ++i) {
    uintptr_t f = Object(i);
    if (!f || ClassNameOf(f) != "Function") continue;
    sc.funcNames.insert(ObjNameIndex(f));
    if (int nat = FuncNative(f)) natives.push_back({nat, f});
    uintptr_t outer = ObjOuter(f);
    std::string cn = ObjName(outer), fn = ObjName(f);
    bool game = StartsWith(cn, "DV") || StartsWith(cn, "NT");
    bool want = false;
    if (custom) {
      for (const std::string& c : customClasses) want |= cn == c;
      for (const std::string& k : customKeys) want |= game && fn.find(k) != std::string::npos;
    } else {
      want = whole.count(cn) || StartsWith(cn, "DVPickupItem") || (game && ScriptKey(fn));
    }
    if (want) {
      queue.push_back({f, 0});
      seen.insert(f);
    }
  }
  size_t done = 0;
  for (size_t qi = 0; qi < queue.size() && qi < (custom ? 8000u : 3000u); ++qi) {
    auto [f, depth] = queue[qi];
    uint32_t fl = FuncFlags(f);
    TArrayRaw s{};
    bool has = StructScript(f, s);
    o.Line("");
    o.Line("### %s  flags=0x%08X [%s] parms=%d/%dB  script=%d bytes%s", PathName(f).c_str(), fl,
           FuncFlagsText(fl).c_str(), FuncNumParms(f), FuncParmsSize(f), has ? s.num : -1,
           depth ? "  (callee)" : "");
    o.Line("    %s", FuncSignature(f).c_str());
    if (!has || s.num <= 0 || !s.data) continue;
    std::vector<uint8_t> code(s.num);
    if (!util::SafeRead(s.data, code.data(), code.size())) continue;
    size_t before = sc.calledFuncs.size();
    for (const std::string& r : ScriptRefs(code, sc)) o.Line("%s", r.c_str());
    for (size_t k = before; k < sc.calledFuncs.size(); ++k) {
      uintptr_t c = sc.calledFuncs[k];
      std::string ocn = ObjName(ObjOuter(c));
      if (depth < 2 && !seen.count(c) && (StartsWith(ocn, "DV") || StartsWith(ocn, "NT"))) {
        seen.insert(c);
        queue.push_back({c, depth + 1});
      }
    }
    size_t lim = std::min<size_t>(code.size(), 16384);
    for (size_t i = 0; i < lim; i += 32) {
      char line[160];
      int k = snprintf(line, sizeof line, "    %04X:", (unsigned)i);
      for (size_t j = i; j < i + 32 && j < lim; ++j) k += snprintf(line + k, sizeof line - k, " %02X", code[j]);
      o.Line("%s", line);
    }
    ++done;
  }
  o.Line("");
  o.Line("##### natives (iNative -> function)");
  std::sort(natives.begin(), natives.end());
  for (auto& [nat, f] : natives)
    o.Line("native %d %s parms=%d flags=0x%08X", nat, PathName(f).c_str(), FuncNumParms(f), FuncFlags(f));
  SetResult(o.Name() + ": " + std::to_string(queue.size()) + " functions (" + std::to_string(done) +
            " with bytecode)");
}

void DumpLayout() {
  Out o(L"layout.txt");
  Header(o, "layout + signatures");
  o.Line("%s", LayoutReport().c_str());
  o.Line("%s", SignaturesReport().c_str());
  SetResult(o.Name() + " written");
}

}  // namespace

void RunAsync(Kind kind, const std::string& arg) {
  bool expected = false;
  if (!g_busy.compare_exchange_strong(expected, true)) return;
  std::thread([kind, arg] {
    g_arg = arg;
    SetResult("working...");
    if (!ue3::Init()) {
      SetResult("UE3 not ready: " + ue3::StatusText());
    } else {
      DWORD t0 = GetTickCount();
      LOG("dump: start kind %d", (int)kind);
      switch (kind) {
        case Kind::Names: DumpNames(); break;
        case Kind::Objects: DumpObjects(); break;
        case Kind::Classes: DumpClasses(); break;
        case Kind::Profile: DumpProfile(); break;
        case Kind::Layout: DumpLayout(); break;
        case Kind::Scripts: DumpScripts(); break;
      }
      LOG("dump: took %lu ms", GetTickCount() - t0);
    }
    g_busy = false;
  }).detach();
}

bool Busy() { return g_busy; }

std::string LastResult() {
  std::lock_guard<std::mutex> lk(g_resMu);
  return g_result;
}

}  // namespace dumper
