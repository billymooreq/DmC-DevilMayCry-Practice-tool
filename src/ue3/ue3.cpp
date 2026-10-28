#include "ue3/ue3.h"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "log.h"
#include "util.h"

using util::Read;
using util::ReadOr;

namespace ue3 {
namespace {

Layout g_L;
std::recursive_mutex g_mu;
bool g_done = false;
std::atomic<bool> g_ok{false};  // set only when Init has fully finished
std::mutex g_statusMu;  // separate lock: the menu must never wait for a scan
std::string g_status = "not scanned";

void SetStatus(const std::string& st) {
  std::lock_guard<std::mutex> lk(g_statusMu);
  g_status = st;
}

// Name cache has its own short-lived lock: the game thread (ProcessEvent log) reads names while a scan
// may hold g_mu for seconds.
std::mutex g_nameMu;
std::vector<std::string> g_nameCache;
std::unordered_map<std::string, int> g_nameIndex;

struct Section {
  uintptr_t start, size;
  bool exec, write;
};

std::vector<Section> ModuleSections() {
  std::vector<Section> out;
  auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
  auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
  const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
  for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
    out.push_back({base + s[i].VirtualAddress, s[i].Misc.VirtualSize,
                   (s[i].Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0,
                   (s[i].Characteristics & IMAGE_SCN_MEM_WRITE) != 0});
  }
  return out;
}

// Copies a range page by page; unreadable pages become zeros.
std::vector<uint8_t> Snapshot(uintptr_t start, size_t size) {
  std::vector<uint8_t> buf(size, 0);
  for (size_t off = 0; off < size; off += 0x1000) {
    size_t n = std::min<size_t>(0x1000, size - off);
    util::SafeRead(start + off, buf.data() + off, n);
  }
  return buf;
}

bool ReadArr(uintptr_t addr, TArrayRaw& a) { return Read(addr, a); }

// "8B 0D ?? ?? ..." -> matches in the exe's code sections (at most maxHits; counting stops there).
std::vector<uintptr_t> SigScanHits(const char* sig, size_t maxHits) {
  std::vector<uintptr_t> hits;
  std::vector<int> pat;
  for (const char* p = sig; *p;) {
    if (*p == ' ') { ++p; continue; }
    if (*p == '?') { pat.push_back(-1); p += (p[1] == '?') ? 2 : 1; continue; }
    pat.push_back((int)strtoul(p, nullptr, 16));
    p += 2;
  }
  for (const Section& s : ModuleSections()) {
    if (!s.exec || s.size < pat.size()) continue;
    const uint8_t* b = reinterpret_cast<const uint8_t*>(s.start);
    for (size_t i = 0; i + pat.size() <= s.size; ++i) {
      size_t k = 0;
      while (k < pat.size() && (pat[k] < 0 || b[i + k] == pat[k])) ++k;
      if (k == pat.size()) {
        hits.push_back(s.start + i);
        if (hits.size() >= maxHits) return hits;
      }
    }
  }
  return hits;
}

uintptr_t SigScanImpl(const char* sig) {
  std::vector<uintptr_t> h = SigScanHits(sig, 1);
  return h.empty() ? 0 : h[0];
}

// latest_2025 signatures (unique in .text; see CLAUDE.md):
//   FName::Hardcode: mov ecx,[Names.Num]; mov edi,[Names.Data]      -> Data address at +8
//   GObjObjects use: mov ebp,[GObjects.Num]; add esp,20h; mov eax,[GObjects.Data] -> at +10
constexpr const char* kSigGNames = "8B 0D ?? ?? ?? ?? 8B 3D ?? ?? ?? ?? 83 C4 04 89";
constexpr const char* kSigGObjects = "8B 2D ?? ?? ?? ?? 83 C4 20 A1 ?? ?? ?? ?? 83 3C B0 00";

bool StrAt(uintptr_t p, const char* s, bool wide) {
  if (wide) return util::ReadWString(p, 64) == util::Utf8ToWide(s);
  return util::ReadCString(p, 64) == s;
}

uintptr_t NameEntry(int i) {
  TArrayRaw a;
  if (!ReadArr(g_L.gnames, a) || i < 0 || i >= a.num) return 0;
  return ReadOr<uintptr_t>(a.data + 4 * i);
}

std::string ReadNameLive(int i) {
  uintptr_t e = NameEntry(i);
  if (!e) return {};
  bool wide = g_L.nameWide;
  if (g_L.nameIndexShifted && g_L.nameEntryIndex >= 0)
    wide = (ReadOr<int32_t>(e + g_L.nameEntryIndex) & 1) != 0;
  if (wide) return util::WideToUtf8(util::ReadWString(e + g_L.nameEntryStr, 1024));
  return util::ReadCString(e + g_L.nameEntryStr, 1024);
}

// ---------------------------------------------------------------- GNames
bool FindNames(const std::vector<TArrayRaw>& cands, const std::vector<uintptr_t>& addrs) {
  for (size_t c = 0; c < cands.size(); ++c) {
    const TArrayRaw& a = cands[c];
    // Entries come from a bump allocator without alignment (DmC: "None" entry is 0x15 bytes).
    uintptr_t e0 = ReadOr<uintptr_t>(a.data), e1 = ReadOr<uintptr_t>(a.data + 4);
    if (!util::PlausibleAddr(e0) || !util::PlausibleAddr(e1)) continue;
    for (int off = 0; off <= 0x20; off += 4) {
      for (int w = 0; w < 2; ++w) {
        if (!StrAt(e0 + off, "None", w) || !StrAt(e1 + off, "ByteProperty", w)) continue;
        g_L.gnames = addrs[c];
        g_L.nameEntryStr = off;
        g_L.nameWide = w != 0;
        // index field: plain i, or (i << 1) | flag
        for (int io = 0; io < off; io += 4) {
          bool plain = true, shifted = true;
          for (int i = 1; i < 64; ++i) {
            int32_t v = ReadOr<int32_t>(ReadOr<uintptr_t>(a.data + 4 * i) + io, -1);
            if (v != i) plain = false;
            if ((v >> 1) != i) shifted = false;
          }
          if (plain || shifted) {
            g_L.nameEntryIndex = io;
            g_L.nameIndexShifted = !plain;
            break;
          }
        }
        LOG("ue3: GNames at %s (RVA 0x%X) num=%d, entry str@+0x%X %s, index@+0x%X%s", util::Hex(addrs[c]).c_str(),
            (unsigned)(addrs[c] - (uintptr_t)GetModuleHandleW(nullptr)), a.num, off, w ? "wide" : "ansi",
            g_L.nameEntryIndex, g_L.nameIndexShifted ? " (shifted)" : "");
        return true;
      }
    }
  }
  return false;
}

// ---------------------------------------------------------------- GObjects
bool FindObjects(const std::vector<TArrayRaw>& cands, const std::vector<uintptr_t>& addrs) {
  int best = -1, bestOff = -1;
  for (size_t c = 0; c < cands.size(); ++c) {
    if (addrs[c] == g_L.gnames) continue;
    const TArrayRaw& a = cands[c];
    // Cheap pre-check: elements 1..3 must share an offset where obj->Index == i (8000+ candidates).
    uint32_t common = 0;
    for (int off = 4, bit = 0; off <= 0x60; off += 4, ++bit) common |= 1u << bit;
    for (int i = 1; i <= 3 && common; ++i) {
      uintptr_t o = ReadOr<uintptr_t>(a.data + 4 * i);
      if (!util::PlausiblePtr(o)) { common = 0; break; }
      for (int off = 4, bit = 0; off <= 0x60; off += 4, ++bit)
        if (ReadOr<int32_t>(o + off, -1) != i) common &= ~(1u << bit);
    }
    if (!common) continue;
    int limit = std::min(a.num, 400);
    std::vector<uintptr_t> objs(limit);
    if (!util::SafeRead(a.data, objs.data(), limit * 4)) continue;
    for (int off = 4; off <= 0x60; off += 4) {
      int checked = 0, match = 0;
      for (int i = 0; i < limit; ++i) {
        if (!objs[i]) continue;
        if (!util::PlausiblePtr(objs[i])) { checked += 1; continue; }
        ++checked;
        if (ReadOr<int32_t>(objs[i] + off, -1) == i) ++match;
      }
      if (checked >= 100 && match * 100 >= checked * 95) {
        if (best < 0 || a.num > cands[best].num) {
          best = (int)c;
          bestOff = off;
        }
        break;
      }
    }
  }
  if (best < 0) {
    // Diagnostics for the next round: the biggest pointer arrays and the head of their 2nd element.
    std::vector<size_t> order(cands.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return cands[a].num > cands[b].num; });
    auto base = (uintptr_t)GetModuleHandleW(nullptr);
    for (size_t k = 0; k < order.size() && k < 12; ++k) {
      const TArrayRaw& a = cands[order[k]];
      uintptr_t e1 = ReadOr<uintptr_t>(a.data + 4);
      uint32_t head[24] = {};
      util::SafeRead(e1, head, sizeof head);
      char line[512];
      int n = snprintf(line, sizeof line, "ue3: cand RVA 0x%X num=%d max=%d e1=0x%08X:", (unsigned)(addrs[order[k]] - base),
                       a.num, a.max, (unsigned)e1);
      for (uint32_t v : head) n += snprintf(line + n, sizeof line - n, " %08X", v);
      LOG("%s", line);
    }
    return false;
  }
  g_L.gobjects = addrs[best];
  g_L.objIndex = bestOff;
  LOG("ue3: GObjects at %s (RVA 0x%X) num=%d, UObject.Index@+0x%X", util::Hex(addrs[best]).c_str(),
      (unsigned)(addrs[best] - (uintptr_t)GetModuleHandleW(nullptr)), cands[best].num, bestOff);
  return true;
}

// ---------------------------------------------------------------- UObject
bool ValidName(const FNameRaw& n) {
  return n.index >= 0 && n.index < NumNames() && n.number >= 0 && n.number < 1000000 && NameEntry(n.index);
}

bool CalibrateObjectName() {
  int cClass = FindNameIndex("Class"), cObject = FindNameIndex("Object"), cCore = FindNameIndex("Core");
  int n = std::min(NumObjects(), 5000);
  for (int off = 0; off <= 0x70; off += 4) {
    if (off == g_L.objIndex) continue;
    int checked = 0, valid = 0;
    bool sClass = false, sObject = false, sCore = false;
    for (int i = 0; i < n; ++i) {
      uintptr_t o = Object(i);
      if (!o) continue;
      ++checked;
      FNameRaw fn{-1, -1};
      Read(o + off, fn);
      if (!ValidName(fn)) continue;
      ++valid;
      sClass |= fn.index == cClass;
      sObject |= fn.index == cObject;
      sCore |= fn.index == cCore;
    }
    if (checked > 100 && valid * 100 >= checked * 98 && sClass && sObject && sCore) {
      g_L.objName = off;
      return true;
    }
  }
  return false;
}

bool CalibrateObjectClass() {
  int cClass = FindNameIndex("Class");
  int n = std::min(NumObjects(), 3000);
  for (int off = 0; off <= 0x70; off += 4) {
    if (off == g_L.objIndex || off == g_L.objName || off == g_L.objName + 4) continue;
    int checked = 0, ok = 0;
    for (int i = 0; i < n; ++i) {
      uintptr_t o = Object(i);
      if (!o) continue;
      ++checked;
      uintptr_t c = ReadOr<uintptr_t>(o + off);
      if (!IsObject(c)) continue;
      uintptr_t cc = ReadOr<uintptr_t>(c + off);
      if (IsObject(cc) && ReadOr<int32_t>(cc + g_L.objName, -1) == cClass) ++ok;
    }
    if (checked > 100 && ok * 100 >= checked * 98) {
      g_L.objClass = off;
      return true;
    }
  }
  return false;
}

uintptr_t FindByNameAndClass(const char* name, const char* cls) {
  int ni = FindNameIndex(name);
  if (ni < 0) return 0;
  int n = NumObjects();
  for (int i = 0; i < n; ++i) {
    uintptr_t o = Object(i);
    if (!o || ReadOr<int32_t>(o + g_L.objName, -1) != ni) continue;
    if (ClassNameOf(o) == cls) return o;
  }
  return 0;
}

bool CalibrateObjectOuter() {
  uintptr_t objCls = FindByNameAndClass("Object", "Class");
  uintptr_t actorCls = FindByNameAndClass("Actor", "Class");
  if (!objCls || !actorCls) return false;
  for (int off = 0; off <= 0x70; off += 4) {
    if (off == g_L.objIndex || off == g_L.objName || off == g_L.objName + 4 || off == g_L.objClass) continue;
    uintptr_t p1 = ReadOr<uintptr_t>(objCls + off), p2 = ReadOr<uintptr_t>(actorCls + off);
    if (!IsObject(p1) || !IsObject(p2)) continue;
    if (ObjName(p1) == "Core" && ObjName(p2) == "Engine" && ClassNameOf(p1) == "Package" &&
        ClassNameOf(p2) == "Package") {
      g_L.objOuter = off;
      return true;
    }
  }
  return false;
}

uintptr_t FindChild(const char* name, uintptr_t outer) {
  int ni = FindNameIndex(name);
  int n = NumObjects();
  for (int i = 0; i < n; ++i) {
    uintptr_t o = Object(i);
    if (o && ReadOr<int32_t>(o + g_L.objName, -1) == ni && ObjOuter(o) == outer) return o;
  }
  return 0;
}

bool Reserved(int off) {
  return off == g_L.objIndex || off == g_L.objName || off == g_L.objName + 4 || off == g_L.objClass ||
         off == g_L.objOuter;
}

// ---------------------------------------------------------------- fields / structs / properties
void CalibrateFunction();

void CalibratePropFlags(uintptr_t X, uintptr_t Y, uintptr_t Z) {
  if (g_L.funcNumParms < 0) CalibrateFunction();  // needs NumParms
  if (g_L.funcNumParms < 0 || g_L.propOffset < 0) return;
  // params of functions that have some
  std::vector<std::pair<uintptr_t, int>> funcs;
  int n = NumObjects();
  for (int i = 0; i < n && funcs.size() < 300; ++i) {
    uintptr_t o = Object(i);
    if (!o || ClassNameOf(o) != "Function") continue;
    int np = ReadOr<uint8_t>(o + g_L.funcNumParms);
    if (np > 0 && np < 16) funcs.push_back({o, np});
  }
  // DmC keeps PropertyFlags (qword) right after UField, *before* ArrayDim (+0x3C), so search the whole
  // UProperty head, skipping ArrayDim/ElementSize/Offset themselves.
  for (int off = g_L.fieldNext + 4; off < g_L.propOffset; off += 4) {
    if (off == g_L.propArrayDim || off == g_L.propElementSize) continue;
    bool ok = true;
    for (uintptr_t p : {X, Y, Z}) ok &= (ReadOr<uint32_t>(p + off) & 0x80) == 0;
    int good = 0, total = 0;
    for (auto& [f, np] : funcs) {
      uintptr_t c = StructChildren(f);
      for (int k = 0; k < np && c; ++k, c = FieldNext(c)) {
        ++total;
        good += (ReadOr<uint32_t>(c + off) & 0x80) != 0;
      }
    }
    if (ok && total > 50 && good * 100 >= total * 98) {
      g_L.propFlags = off;
      return;
    }
  }
  LOG("ue3: PropertyFlags not calibrated, keeping +0x%X", g_L.propFlags);
}

void CalibrateFields() {
  uintptr_t vec = FindObject("ScriptStruct", "Vector", "Object");
  uintptr_t plane = FindObject("ScriptStruct", "Plane", "Object");
  if (!vec || !plane) {
    LOG("ue3: Core.Object.Vector/Plane not found, field layout unknown");
    return;
  }
  uintptr_t X = FindChild("X", vec), Y = FindChild("Y", vec), Z = FindChild("Z", vec), W = FindChild("W", plane);
  if (!X || !Y || !Z || !W) {
    LOG("ue3: Vector.X/Y/Z or Plane.W not found");
    return;
  }
  for (int off = 0x20; off <= 0x80; off += 4) {
    if (Reserved(off)) continue;
    auto rd = [&](uintptr_t o) { return ReadOr<uintptr_t>(o + off); };
    if ((rd(X) == Y && rd(Y) == Z) || (rd(Z) == Y && rd(Y) == X)) {
      g_L.fieldNext = off;
      break;
    }
  }
  if (g_L.fieldNext < 0) return;
  int from = g_L.fieldNext + 4;
  for (int off = from; off <= 0xA0; off += 4) {
    uintptr_t c = ReadOr<uintptr_t>(vec + off);
    if (g_L.structChildren < 0 && (c == X || c == Z)) g_L.structChildren = off;
    if (g_L.structSuper < 0 && ReadOr<uintptr_t>(plane + off) == vec) g_L.structSuper = off;
  }
  for (int off = std::max(g_L.structChildren, g_L.structSuper) + 4; off <= 0xC0; off += 4) {
    if (ReadOr<int32_t>(vec + off) == 12 && ReadOr<int32_t>(plane + off) == 16) {
      g_L.structSize = off;
      break;
    }
  }
  // UProperty: ArrayDim == 1, ElementSize == 4 for X/Y/Z; Offset 0/4/8 (and W == 12).
  for (int off = from; off <= 0x80; off += 4) {
    bool ok = true;
    for (uintptr_t p : {X, Y, Z, W})
      ok &= ReadOr<int32_t>(p + off) == 1 && ReadOr<int32_t>(p + off + 4) == 4;
    if (ok) {
      g_L.propArrayDim = off;
      g_L.propElementSize = off + 4;
      g_L.propFlags = off + 8;
      break;
    }
  }
  if (g_L.propArrayDim >= 0) {
    for (int off = g_L.propFlags + 8; off <= 0xA0; off += 4) {
      if (ReadOr<int32_t>(X + off, -1) == 0 && ReadOr<int32_t>(Y + off) == 4 && ReadOr<int32_t>(Z + off) == 8 &&
          ReadOr<int32_t>(W + off) == 12) {
        g_L.propOffset = off;
        break;
      }
    }
  }
  // PropertyFlags: the first NumParms children of a function carry CPF_Parm (0x80), struct members don't.
  // (ArrayDim+8 is wrong in DmC: there is extra data between ElementSize and the flags.)
  CalibratePropFlags(X, Y, Z);
  // First subclass member: Actor.Location (StructProperty) -> Struct == Vector.
  uintptr_t actor = FindObject("Class", "Actor", "Engine");
  uintptr_t loc = actor ? FindChild("Location", actor) : 0;
  if (loc && g_L.propOffset >= 0) {
    for (int off = g_L.propOffset + 4; off <= 0x100; off += 4) {
      if (ReadOr<uintptr_t>(loc + off) == vec) {
        g_L.propExtra = off;
        break;
      }
    }
  }
  // UClass default object
  if (actor) {
    int def = FindNameIndex("Default__Actor");
    for (int off = 0x40; off <= 0x400; off += 4) {
      uintptr_t p = ReadOr<uintptr_t>(actor + off);
      if (IsObject(p) && ObjNameIndex(p) == def) {
        g_L.classDefaultObject = off;
        break;
      }
    }
  }
}

void CalibrateEnum() {
  if (g_L.fieldNext < 0) return;
  std::vector<uintptr_t> enums;
  int n = NumObjects();
  for (int i = 0; i < n && enums.size() < 20; ++i) {
    uintptr_t o = Object(i);
    if (o && ClassNameOf(o) == "Enum") enums.push_back(o);
  }
  for (int off = g_L.fieldNext + 4; off <= g_L.fieldNext + 0x20; off += 4) {
    int good = 0;
    for (uintptr_t e : enums) {
      TArrayRaw a{};
      if (!Read(e + off, a) || a.num < 1 || a.num > 4096 || a.max < a.num || !util::PlausiblePtr(a.data)) continue;
      bool ok = true;
      for (int k = 0; k < a.num && ok; ++k) ok = ValidName(ReadOr<FNameRaw>(a.data + 8 * k, {-1, -1}));
      good += ok;
    }
    if (!enums.empty() && good * 10 >= (int)enums.size() * 9) {
      g_L.enumNames = off;
      return;
    }
  }
}

void CalibrateFunction() {
  std::vector<uintptr_t> funcs;
  int n = NumObjects();
  for (int i = 0; i < n && funcs.size() < 3000; ++i) {
    uintptr_t o = Object(i);
    if (o && ClassNameOf(o) == "Function") funcs.push_back(o);
  }
  if (funcs.empty()) return;
  int from = g_L.structSize > 0 ? g_L.structSize + 4 : 0x40;
  for (int off = from; off <= 0x140; off += 4) {
    int same = 0;
    for (uintptr_t f : funcs) {
      FNameRaw a = ReadOr<FNameRaw>(f + g_L.objName, {-1, -1}), b = ReadOr<FNameRaw>(f + off, {-2, -2});
      same += (a.index == b.index && a.number == b.number);
    }
    if (same * 10 >= (int)funcs.size() * 8) {
      g_L.funcFriendlyName = off;
      g_L.funcFlags = off - 8;
      g_L.funcNative = off - 4;
      g_L.funcNumParms = off + 9;
      g_L.funcParmsSize = off + 10;
      g_L.funcRetOffset = off + 12;
      return;
    }
  }
  // Fallback: exec bit (0x200) on most CheatManager functions.
  uintptr_t cm = FindObject("Class", "CheatManager", nullptr);
  if (!cm) return;
  std::vector<uintptr_t> cmf;
  for (uintptr_t f : funcs)
    if (ObjOuter(f) == cm) cmf.push_back(f);
  for (int off = from; off <= 0x140 && !cmf.empty(); off += 4) {
    int ex = 0;
    for (uintptr_t f : cmf) {
      uint32_t v = ReadOr<uint32_t>(f + off);
      ex += (v & 0x200) && v < 0x08000000;
    }
    if (ex * 10 >= (int)cmf.size() * 6) {
      g_L.funcFlags = off;
      g_L.funcNative = off + 4;
      return;
    }
  }
}

// UStruct::Script (TArray<BYTE>) is the FIRST TArray after PropertySize that is non-empty for defined script
// functions (DmC: +0x50, confirmed by FFrame::FFrame comparing CodeOffset with [Node+54h]). A later TArray
// (+0x74, ScriptObjectReferences) matches the same test, so take the first hit.
// Also UFunction::Func (native thunk): pointer into the exe's code for native functions (DmC: +0xA8).
void CalibrateScript() {
  if (g_L.structSize < 0 || g_L.funcFlags < 0) return;
  std::vector<uintptr_t> defined, natives;
  int n = NumObjects();
  for (int i = 0; i < n && (defined.size() < 500 || natives.size() < 200); ++i) {
    uintptr_t o = Object(i);
    if (!o || ClassNameOf(o) != "Function") continue;
    uint32_t fl = ReadOr<uint32_t>(o + g_L.funcFlags);
    if ((fl & 0x400) && natives.size() < 200) natives.push_back(o);
    else if ((fl & 0x2) && !(fl & 0x400) && defined.size() < 500) defined.push_back(o);
  }
  for (int off = g_L.structSize + 4; off + 12 <= g_L.funcFlags; off += 4) {
    int good = 0;
    for (uintptr_t f : defined) {
      TArrayRaw a{};
      good += Read(f + off, a) && a.num > 0 && a.num < 0x100000 && a.max >= a.num && util::PlausiblePtr(a.data);
    }
    if (!defined.empty() && good * 10 >= (int)defined.size() * 9) {
      g_L.structScript = off;
      break;
    }
  }
  uintptr_t textLo = 0, textHi = 0;
  for (const Section& s : ModuleSections())
    if (s.exec && !textLo) {
      textLo = s.start;
      textHi = s.start + s.size;
    }
  for (int off = (g_L.funcRetOffset > 0 ? g_L.funcRetOffset + 2 : g_L.funcFlags + 4); off <= 0x120; off += 2) {
    if (off & 3) continue;
    int inText = 0;
    for (uintptr_t f : natives) {
      uintptr_t p = ReadOr<uintptr_t>(f + off);
      inText += p >= textLo && p < textHi;
    }
    if (!natives.empty() && inText * 10 >= (int)natives.size() * 9) {
      g_L.funcFunc = off;
      break;
    }
  }
}

std::string CodeRefs(uintptr_t target, int maxHits) {
  std::string out;
  int hits = 0;
  for (const Section& s : ModuleSections()) {
    if (!s.exec) continue;
    std::vector<uint8_t> code = Snapshot(s.start, s.size);
    for (size_t i = 8; i + 12 < code.size() && hits < maxHits; ++i) {
      if (*reinterpret_cast<uint32_t*>(&code[i]) != (uint32_t)target) continue;
      ++hits;
      char line[256];
      int k = snprintf(line, sizeof line, "  RVA 0x%08X: ",
                       (unsigned)(s.start + i - (uintptr_t)GetModuleHandleW(nullptr)));
      for (size_t j = i - 8; j < i + 8; ++j) {
        bool wild = j >= i && j < i + 4;
        k += snprintf(line + k, sizeof line - k, wild ? "?? " : "%02X ", code[j]);
      }
      out += line;
      out += "\n";
    }
  }
  if (!hits) out = "  (no absolute references found)\n";
  return out;
}

}  // namespace

const Layout& L() { return g_L; }

bool Ready() { return g_ok; }

std::string StatusText() {
  std::lock_guard<std::mutex> lk(g_statusMu);
  return g_status;
}

void Reset() {
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  g_done = false;
  g_ok = false;
  g_L = Layout{};
  std::lock_guard<std::mutex> nl(g_nameMu);
  g_nameCache.clear();
  g_nameIndex.clear();
  SetStatus("not scanned");
}

namespace {

// Layout parts the game features rely on, empty when complete.
std::string MissingLayouts() {
  const Layout& l = g_L;
  std::string m;
  auto need = [&m](int v, const char* what) {
    if (v < 0) m += (m.empty() ? "" : ", ") + std::string(what);
  };
  need(l.fieldNext, "UField.Next");
  need(l.structChildren, "UStruct.Children");
  need(l.structSuper, "UStruct.SuperField");
  need(l.propOffset, "UProperty.Offset");
  need(l.propFlags, "UProperty.Flags");
  need(l.propExtra, "UProperty subclass");
  need(l.funcFlags, "UFunction.Flags");
  need(l.funcFunc, "UFunction.Func");
  need(l.classDefaultObject, "UClass.CDO");
  return m;
}

// Signature path is usable: both TArrays read and already hold the bulk of the startup packages.
bool SignaturesLookReady(std::string* why) {
  uintptr_t sn = SigScanImpl(kSigGNames), so = SigScanImpl(kSigGObjects);
  if (!sn || !so) {
    *why = "signatures not found (other game build?)";
    return false;
  }
  TArrayRaw n{}, o{};
  if (!ReadArr(ReadOr<uintptr_t>(sn + 8), n) || !ReadArr(ReadOr<uintptr_t>(so + 10), o)) {
    *why = "GNames/GObjects not readable yet";
    return false;
  }
  if (n.num < 1000 || o.num < 1000) {
    char b[96];
    snprintf(b, sizeof b, "game still loading (%d names, %d objects)", n.num, o.num);
    *why = b;
    return false;
  }
  return true;
}

}  // namespace

bool Init(bool background) {
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  if (g_done && g_ok) return true;
  if (g_done) {  // previous attempt failed (e.g. too early): start over
    g_L = Layout{};
    std::lock_guard<std::mutex> nl(g_nameMu);
    g_nameCache.clear();
    g_nameIndex.clear();
  }
  g_done = true;
  ULONGLONG t0 = GetTickCount64();
  SetStatus("scanning...");
  LOG("ue3: scanning%s...", background ? " (auto)" : "");

  auto buildNameCache = [] {
    int nn = NumNames();
    std::vector<std::string> cache;
    std::unordered_map<std::string, int> index;
    cache.reserve(nn);
    for (int i = 0; i < nn; ++i) {
      cache.push_back(ReadNameLive(i));
      index.emplace(cache.back(), i);
    }
    std::lock_guard<std::mutex> nl(g_nameMu);
    g_nameCache.swap(cache);
    g_nameIndex.swap(index);
  };

  // Fast path: signatures give the TArray addresses directly.
  bool viaSig = false;
  uintptr_t sn = SigScanImpl(kSigGNames), so = SigScanImpl(kSigGObjects);
  if (sn && so) {
    std::vector<TArrayRaw> c;
    std::vector<uintptr_t> a;
    for (uintptr_t addr : {ReadOr<uintptr_t>(sn + 8), ReadOr<uintptr_t>(so + 10)}) {
      TArrayRaw t{};
      if (ReadArr(addr, t)) {
        c.push_back(t);
        a.push_back(addr);
      }
    }
    LOG("ue3: signatures matched (GNames sig @%p, GObjects sig @%p)", (void*)sn, (void*)so);
    if (FindNames(c, a)) {
      buildNameCache();
      viaSig = FindObjects(c, a);
    }
    if (!viaSig && background) {
      SetStatus("waiting for the game to load (signature addresses not valid yet)");
      LOG("ue3: signature addresses did not validate yet, will retry");
      g_L = Layout{};
      std::lock_guard<std::mutex> nl(g_nameMu);
      g_nameCache.clear();
      g_nameIndex.clear();
      return false;
    }
    if (!viaSig) {
      LOG("ue3: signature addresses did not validate, falling back to a full scan");
      g_L = Layout{};
      std::lock_guard<std::mutex> nl(g_nameMu);
      g_nameCache.clear();
      g_nameIndex.clear();
    }
  } else if (background) {
    SetStatus("signatures not found (other game build?)");
    return false;
  } else {
    LOG("ue3: signatures not found (other build?), full scan");
  }

  if (!viaSig) {
    // TArray candidates {Data, Num, Max} in writable sections of the exe.
    std::vector<TArrayRaw> cands;
    std::vector<uintptr_t> addrs;
    for (const Section& s : ModuleSections()) {
      if (!s.write || s.exec) continue;
      std::vector<uint8_t> buf = Snapshot(s.start, s.size);
      for (size_t i = 0; i + 12 <= buf.size(); i += 4) {
        TArrayRaw t;
        memcpy(&t, &buf[i], sizeof t);
        if (t.num < 1000 || t.num > t.max || t.max > 0x2000000 || !util::PlausiblePtr(t.data)) continue;
        cands.push_back(t);
        addrs.push_back(s.start + i);
      }
    }
    LOG("ue3: %zu TArray candidates", cands.size());
    if (!FindNames(cands, addrs)) {
      SetStatus("GNames not found (game not initialised yet? press Rescan in the main menu)");
      LOG("ue3: GNames not found");
      return false;
    }
    buildNameCache();
    if (!FindObjects(cands, addrs)) {
      SetStatus("GObjects not found");
      LOG("ue3: GObjects not found");
      return false;
    }
  }
  LOG("ue3: calibrating UObject (%d names cached)", (int)g_nameCache.size());
  if (!CalibrateObjectName() || !CalibrateObjectClass() || !CalibrateObjectOuter()) {
    SetStatus("UObject layout not resolved");
    LOG("ue3: UObject layout not resolved (name=%d class=%d outer=%d)", g_L.objName, g_L.objClass, g_L.objOuter);
    return false;
  }
  LOG("ue3: UObject Name +0x%X Class +0x%X Outer +0x%X; calibrating fields", g_L.objName, g_L.objClass, g_L.objOuter);
  CalibrateFields();
  LOG("ue3: calibrating UEnum");
  CalibrateEnum();
  LOG("ue3: calibrating UFunction");
  if (g_L.funcFriendlyName < 0) CalibrateFunction();  // usually already done for PropertyFlags
  CalibrateScript();
  std::string missing = MissingLayouts();
  if (background && !missing.empty()) {  // classes like Engine.Actor not loaded yet: try again later
    SetStatus("layout incomplete, retrying (" + missing + ")");
    LOG("ue3: layout incomplete (%s), will retry", missing.c_str());
    return false;
  }
  LOG("%s", LayoutReport().c_str());
  char b[256];
  snprintf(b, sizeof b, "OK: %d names, %d objects%s%s", NumNames(), NumObjects(), missing.empty() ? "" : "; missing: ",
           missing.c_str());
  SetStatus(b);
  LOG("ue3: ready (%s, %d names, %d objects, %llu ms%s%s)", viaSig ? "signatures" : "full scan", NumNames(),
      NumObjects(), (unsigned long long)(GetTickCount64() - t0), missing.empty() ? "" : "; missing: ", missing.c_str());
  g_ok = true;
  return true;
}

void StartAutoInit() {
  static std::atomic<bool> started{false};
  if (started.exchange(true)) return;
  std::thread([] {
    const ULONGLONG t0 = GetTickCount64();
    const ULONGLONG kWarnMs = 60000;  // long enough for a cold start to reach the title screen
    bool warned = false, fullScanTried = false;
    int failures = 0;
    std::string why;
    while (!g_ok) {
      ULONGLONG el = GetTickCount64() - t0;
      bool ready = SignaturesLookReady(&why);
      if (ready) {
        if (Init(true)) break;
        why = StatusText();
        ++failures;
      } else if (el > kWarnMs && !fullScanTried && why.rfind("signatures not found", 0) == 0) {
        fullScanTried = true;  // other build: one slow full scan, the sig path cannot work
        LOG("ue3: auto-init: %s, trying a full scan", why.c_str());
        if (Init(false)) break;
        why = StatusText();
      }
      if (!warned && el > kWarnMs) {
        warned = true;
        LOG("ue3: NOT ready after %llu s: %s (still retrying)", (unsigned long long)(el / 1000), why.c_str());
      }
      if (failures > 0 && !ready) failures = 0;
      // A failed Init means the arrays are there but classes are still loading: back off a little.
      Sleep(failures ? std::min(1000 * failures, 5000) : 1000);
      if (failures >= 60) {
        LOG("ue3: auto-init gave up after %d attempts: %s (Dev -> Rescan UE3)", failures, why.c_str());
        break;
      }
    }
    if (g_ok) LOG("ue3: auto-init done after %llu ms", (unsigned long long)(GetTickCount64() - t0));
  }).detach();
}

std::string LayoutReport() {
  const Layout& l = g_L;
  auto base = (uintptr_t)GetModuleHandleW(nullptr);
  char b[2048];
  snprintf(b, sizeof b,
           "ue3 layout:\n"
           "  GNames   %s RVA 0x%X  FNameEntry: str +0x%X (%s), index +0x%X%s\n"
           "  GObjects %s RVA 0x%X\n"
           "  UObject  Index +0x%X  Name +0x%X  Class +0x%X  Outer +0x%X\n"
           "  UField   Next +0x%X\n"
           "  UStruct  SuperField +0x%X  Children +0x%X  PropertySize +0x%X  Script +0x%X\n"
           "  UProperty ArrayDim +0x%X  ElementSize +0x%X  PropertyFlags +0x%X  Offset +0x%X  subclass +0x%X\n"
           "  UEnum    Names +0x%X\n"
           "  UFunction FunctionFlags +0x%X  iNative +0x%X  FriendlyName +0x%X  NumParms +0x%X  ParmsSize +0x%X  RetOffset +0x%X  Func +0x%X\n"
           "  UClass   ClassDefaultObject +0x%X\n",
           util::Hex(l.gnames).c_str(), (unsigned)(l.gnames ? l.gnames - base : 0), l.nameEntryStr,
           l.nameWide ? "wide" : "ansi", l.nameEntryIndex, l.nameIndexShifted ? " shifted" : "",
           util::Hex(l.gobjects).c_str(), (unsigned)(l.gobjects ? l.gobjects - base : 0), l.objIndex, l.objName,
           l.objClass, l.objOuter, l.fieldNext, l.structSuper, l.structChildren, l.structSize, l.structScript, l.propArrayDim,
           l.propElementSize, l.propFlags, l.propOffset, l.propExtra, l.enumNames, l.funcFlags, l.funcNative,
           l.funcFriendlyName, l.funcNumParms, l.funcParmsSize, l.funcRetOffset, l.funcFunc, l.classDefaultObject);
  return b;
}

std::string SignaturesReport() {
  if (!g_L.gnames || !g_L.gobjects) return "not scanned\n";
  return "Code references to GNames (TArray address, bytes masked with ??):\n" + CodeRefs(g_L.gnames, 16) +
         "Code references to GObjects:\n" + CodeRefs(g_L.gobjects, 16) +
         "Code references to GObjects.Num (+4):\n" + CodeRefs(g_L.gobjects + 4, 8);
}

// ---------------------------------------------------------------- names
int NumNames() {
  TArrayRaw a{};
  return (g_L.gnames && ReadArr(g_L.gnames, a)) ? a.num : 0;
}

std::string Name(int i) {
  {
    std::lock_guard<std::mutex> lk(g_nameMu);
    if (i >= 0 && i < (int)g_nameCache.size()) return g_nameCache[i];
  }
  return ReadNameLive(i);
}

std::string NameOf(const FNameRaw& n) {
  std::string s = Name(n.index);
  if (n.number > 0) s += "_" + std::to_string(n.number - 1);
  return s;
}

std::string ReadFName(uintptr_t addr) {
  FNameRaw n{-1, 0};
  if (!Read(addr, n) || n.index < 0) return "<bad name>";
  return NameOf(n);
}

int FindNameIndex(const char* s) {
  {
    std::lock_guard<std::mutex> lk(g_nameMu);
    auto it = g_nameIndex.find(s);
    if (it != g_nameIndex.end()) return it->second;
  }
  // GNames only grows (packages load later than the first scan): append the new tail and retry.
  static std::mutex catchUp;
  std::lock_guard<std::mutex> cu(catchUp);
  size_t have;
  {
    std::lock_guard<std::mutex> lk(g_nameMu);
    have = g_nameCache.size();
  }
  int total = NumNames();
  if (have == 0 || (int)have >= total) return -1;
  std::vector<std::string> tail;
  tail.reserve(total - have);
  for (int i = (int)have; i < total; ++i) tail.push_back(ReadNameLive(i));
  std::lock_guard<std::mutex> lk(g_nameMu);
  for (size_t k = 0; k < tail.size(); ++k) {
    g_nameCache.push_back(tail[k]);
    g_nameIndex.emplace(tail[k], (int)(have + k));
  }
  auto it = g_nameIndex.find(s);
  return it == g_nameIndex.end() ? -1 : it->second;
}

// ---------------------------------------------------------------- objects
int NumObjects() {
  TArrayRaw a{};
  return (g_L.gobjects && ReadArr(g_L.gobjects, a)) ? a.num : 0;
}

uintptr_t Object(int i) {
  TArrayRaw a{};
  if (!ReadArr(g_L.gobjects, a) || i < 0 || i >= a.num) return 0;
  return ReadOr<uintptr_t>(a.data + 4 * i);
}

bool IsObject(uintptr_t p) {
  if (!util::PlausiblePtr(p) || g_L.objIndex < 0) return false;
  int32_t idx = ReadOr<int32_t>(p + g_L.objIndex, -1);
  return idx >= 0 && Object(idx) == p;
}

int ObjNameIndex(uintptr_t o) { return ReadOr<int32_t>(o + g_L.objName, -1); }
std::string ObjName(uintptr_t o) { return o ? ReadFName(o + g_L.objName) : "None"; }
uintptr_t ObjClass(uintptr_t o) { return ReadOr<uintptr_t>(o + g_L.objClass); }
uintptr_t ObjOuter(uintptr_t o) { return g_L.objOuter >= 0 ? ReadOr<uintptr_t>(o + g_L.objOuter) : 0; }
int ObjIndexOf(uintptr_t o) { return ReadOr<int32_t>(o + g_L.objIndex, -1); }
std::string ClassNameOf(uintptr_t o) {
  uintptr_t c = ObjClass(o);
  return c ? ObjName(c) : "None";
}

std::string PathName(uintptr_t o) {
  std::string s = ObjName(o);
  uintptr_t outer = ObjOuter(o);
  for (int depth = 0; outer && depth < 16; ++depth) {
    s = ObjName(outer) + "." + s;
    outer = ObjOuter(outer);
  }
  return s;
}

std::string FullName(uintptr_t o) { return ClassNameOf(o) + " " + PathName(o); }

uintptr_t FindObject(const char* className, const char* name, const char* outerName) {
  int ni = FindNameIndex(name);
  if (ni < 0) return 0;
  int n = NumObjects();
  for (int i = 0; i < n; ++i) {
    uintptr_t o = Object(i);
    if (!o || ObjNameIndex(o) != ni) continue;
    if (className && ClassNameOf(o) != className) continue;
    if (outerName && ObjName(ObjOuter(o)) != outerName) continue;
    return o;
  }
  return 0;
}

bool IsChildOf(uintptr_t s, const char* baseName) {
  int ni = FindNameIndex(baseName);
  for (int depth = 0; s && depth < 64; ++depth) {
    if (ObjNameIndex(s) == ni) return true;
    s = StructSuper(s);
  }
  return false;
}

bool IsA(uintptr_t o, const char* className) { return IsChildOf(ObjClass(o), className); }

// ---------------------------------------------------------------- fields
uintptr_t FieldNext(uintptr_t f) { return g_L.fieldNext >= 0 ? ReadOr<uintptr_t>(f + g_L.fieldNext) : 0; }
uintptr_t StructSuper(uintptr_t s) { return g_L.structSuper >= 0 ? ReadOr<uintptr_t>(s + g_L.structSuper) : 0; }
uintptr_t StructChildren(uintptr_t s) {
  return g_L.structChildren >= 0 ? ReadOr<uintptr_t>(s + g_L.structChildren) : 0;
}
int StructSize(uintptr_t s) { return g_L.structSize >= 0 ? ReadOr<int32_t>(s + g_L.structSize) : 0; }
int PropArrayDim(uintptr_t p) { return ReadOr<int32_t>(p + g_L.propArrayDim); }
int PropElementSize(uintptr_t p) { return ReadOr<int32_t>(p + g_L.propElementSize); }
uint64_t PropFlags(uintptr_t p) { return ReadOr<uint64_t>(p + g_L.propFlags); }
int PropOffset(uintptr_t p) { return ReadOr<int32_t>(p + g_L.propOffset); }
uintptr_t PropExtra(uintptr_t p, int slot) {
  return g_L.propExtra >= 0 ? ReadOr<uintptr_t>(p + g_L.propExtra + 4 * slot) : 0;
}
uint32_t PropExtraU32(uintptr_t p) { return g_L.propExtra >= 0 ? ReadOr<uint32_t>(p + g_L.propExtra) : 0; }

std::vector<std::string> EnumNames(uintptr_t e) {
  std::vector<std::string> out;
  TArrayRaw a{};
  if (g_L.enumNames < 0 || !e || !Read(e + g_L.enumNames, a) || a.num < 0 || a.num > 4096) return out;
  for (int i = 0; i < a.num; ++i) out.push_back(NameOf(ReadOr<FNameRaw>(a.data + 8 * i, {0, 0})));
  return out;
}

uint32_t FuncFlags(uintptr_t f) { return g_L.funcFlags >= 0 ? ReadOr<uint32_t>(f + g_L.funcFlags) : 0; }
uint16_t FuncNative(uintptr_t f) { return g_L.funcNative >= 0 ? ReadOr<uint16_t>(f + g_L.funcNative) : 0; }
int FuncNumParms(uintptr_t f) { return g_L.funcNumParms >= 0 ? ReadOr<uint8_t>(f + g_L.funcNumParms) : -1; }
int FuncParmsSize(uintptr_t f) { return g_L.funcParmsSize >= 0 ? ReadOr<uint16_t>(f + g_L.funcParmsSize) : -1; }
uintptr_t SigScan(const char* sig) { return SigScanImpl(sig); }
std::vector<uintptr_t> SigScanAll(const char* sig, size_t maxHits) { return SigScanHits(sig, maxHits); }

std::vector<SigInfo> Signatures() { return {{"GNames", kSigGNames, 8}, {"GObjects", kSigGObjects, 10}}; }

uintptr_t FuncNativePtr(uintptr_t f) { return g_L.funcFunc >= 0 ? ReadOr<uintptr_t>(f + g_L.funcFunc) : 0; }

bool StructScript(uintptr_t s, TArrayRaw& out) {
  return g_L.structScript >= 0 && Read(s + g_L.structScript, out) && out.num >= 0 && out.num < 0x100000;
}

uintptr_t ClassDefaultObject(uintptr_t cls) {
  return g_L.classDefaultObject >= 0 ? ReadOr<uintptr_t>(cls + g_L.classDefaultObject) : 0;
}

}  // namespace ue3
