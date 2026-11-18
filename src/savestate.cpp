#include "savestate.h"

#include <windows.h>
#include <d3d9.h>
#include <intrin.h>
#include <dbghelp.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <string>
#include <vector>

#include "MinHook.h"
#include "imgui.h"
#include "config.h"
#include "d3d9_hook.h"
#include "game.h"
#include "hooks.h"
#include "log.h"
#include "nav.h"
#include "ue3/engine.h"
#include "ue3/ue3.h"
#include "util.h"
#include "features.h"
#include "menu.h"
#include "savestate_store.h"
#include "timers.h"

// ---------------------------------------------------------------------------------------------------------------
// #28: kept threads (see "Kept threads" below). A game thread a slot knows does not end when the game ends it (a map
// opened again: Restart, Load checkpoint, another mission): at RtlExitUserThread it parks on a private stack of ours
// and waits. A load of that slot sends it back into the wait it sat in at the save: its stack goes back (the same
// stack - it never ended), registers, SEH head, MXCSR / x87 control word, then it jumps to the start of the ntdll wait
// stub it was in, with the arguments on the restored stack: the same wait call again (on the same kernel object: the
// game's close of it was held back too). Plain structs and naked functions at file scope (inline asm, fixed offsets).
struct SsRevive {
  uint32_t esp, eip, ebx, esi, edi, ebp, seh, mxcsr;  // 0 4 8 12 16 20 24 28
  uint16_t fcw, pad;                                   // 32
  volatile long* state;                                // 36: set to 0 (record free) just before the jump
};
struct SsPark {
  HANDLE ev;            // auto-reset: the game thread's command is ready
  volatile long cmd;    // 1 revive, 2 end for real
  SsRevive rev;
};
static_assert(offsetof(SsRevive, eip) == 4 && offsetof(SsRevive, seh) == 24 && offsetof(SsRevive, mxcsr) == 28 &&
                  offsetof(SsRevive, fcw) == 32 && offsetof(SsRevive, state) == 36,
              "SsRevive layout (inline asm)");

static __declspec(naked) void __fastcall ss_ReviveJump(SsRevive* r) {
  __asm {
    mov eax, ecx
    ldmxcsr dword ptr [eax + 28]
    fninit
    fldcw word ptr [eax + 32]
    mov edx, [eax + 24]
    mov fs:[0], edx
    mov ebx, [eax + 8]
    mov esi, [eax + 12]
    mov edi, [eax + 16]
    mov ebp, [eax + 20]
    mov ecx, [eax + 4]
    mov edx, [eax + 0]
    mov eax, [eax + 36]
    mov dword ptr [eax], 0
    mov esp, edx
    jmp ecx
  }
}

// On the private stack: wait for the game thread's command. Returns only for "end for real".
extern "C" void __cdecl ss_ParkLoop(SsPark* p) {
  for (;;) {
    WaitForSingleObject(p->ev, INFINITE);  // the DLL's own import (not the game's patched one)
    long c = InterlockedExchange(&p->cmd, 0);
    if (c == 1) ss_ReviveJump(&p->rev);
    if (c == 2) return;
  }
}

// ecx = top of the private stack, edx = the SsPark. Back on the thread's own stack when ss_ParkLoop returns.
static __declspec(naked) void __fastcall ss_ParkOnStack(uintptr_t top, SsPark* p) {
  __asm {
    push ebp
    mov ebp, esp
    push ebx
    push esi
    push edi
    mov esp, ecx
    push edx
    call ss_ParkLoop
    add esp, 4
    lea esp, [ebp - 12]
    pop edi
    pop esi
    pop ebx
    pop ebp
    ret
  }
}

namespace savestate {
namespace {

// #29: save state slots (masks per object are uint8_t: up to 8 without a format change).
constexpr int kSlots = kSlotCount;

// ---------------------------------------------------------------------------------------------------------------
// Thread names. UE3 names its threads with the MSVC "SetThreadName" exception (0x406D1388), meant for a debugger;
// a vectored handler sees it first. Fixed table, no heap (the exception may come from any thread at any time).
constexpr DWORD kNameException = 0x406D1388;
struct ThreadName {
  DWORD tid;
  char name[40];
};
ThreadName g_names[256];
std::atomic<int> g_nameCount{0};

LONG CALLBACK NameCatcher(EXCEPTION_POINTERS* e) {
  if (e->ExceptionRecord->ExceptionCode != kNameException || e->ExceptionRecord->NumberParameters < 3)
    return EXCEPTION_CONTINUE_SEARCH;
  const ULONG_PTR* p = e->ExceptionRecord->ExceptionInformation;  // {0x1000, name, tid, flags}
  if (p[0] != 0x1000) return EXCEPTION_CONTINUE_SEARCH;
  DWORD tid = (DWORD)p[2] == (DWORD)-1 ? GetCurrentThreadId() : (DWORD)p[2];
  int i = g_nameCount.fetch_add(1);
  if (i < (int)std::size(g_names)) {
    g_names[i].tid = tid;
    util::SafeRead(p[1], g_names[i].name, sizeof g_names[i].name - 1);
    g_names[i].name[sizeof g_names[i].name - 1] = 0;
  }
  return EXCEPTION_CONTINUE_SEARCH;  // the game's own __except swallows it, as without a debugger
}

std::string NameOf(DWORD tid) {
  int n = std::min(g_nameCount.load(), (int)std::size(g_names));
  for (int i = n - 1; i >= 0; --i)
    if (g_names[i].tid == tid) return g_names[i].name;
  return "";
}

// ---------------------------------------------------------------------------------------------------------------
// Who allocated each VirtualAlloc region: KernelBase!VirtualAlloc/VirtualFree hooked from DllMain, the caller's
// return address kept per allocation base. Open addressing, no heap, a spin lock.
struct VaEntry {
  uintptr_t base;    // 0 = empty, 1 = deleted
  uintptr_t caller;  // return address of the VirtualAlloc call
  uint32_t zombie;   // released by the game while a slot needs it: kept reserved (decommitted), see hkVirtualFree
};
constexpr size_t kVaSlots = 1 << 18;
VaEntry g_va[kVaSlots];
volatile LONG g_vaLock = 0;
std::atomic<long> g_vaLive{0}, g_vaTotal{0};

struct SpinLock {
  SpinLock() {
    while (InterlockedCompareExchange(&g_vaLock, 1, 0) != 0) YieldProcessor();
  }
  ~SpinLock() { InterlockedExchange(&g_vaLock, 0); }
};

size_t VaHash(uintptr_t b) { return ((b >> 16) * 2654435761u) & (kVaSlots - 1); }

void VaInsertNoLock(uintptr_t base, uintptr_t caller);
void VaInsert(uintptr_t base, uintptr_t caller) {
  SpinLock lk;
  VaInsertNoLock(base, caller);
}
void VaInsertNoLock(uintptr_t base, uintptr_t caller) {
  size_t i = VaHash(base), tomb = SIZE_MAX;
  for (size_t n = 0; n < kVaSlots; ++n, i = (i + 1) & (kVaSlots - 1)) {
    if (g_va[i].base == base) return;  // already known (MEM_COMMIT inside a reserve)
    if (g_va[i].base == 1 && tomb == SIZE_MAX) tomb = i;
    if (g_va[i].base == 0) {
      size_t at = tomb != SIZE_MAX ? tomb : i;
      g_va[at] = {base, caller, 0};
      ++g_vaLive;
      ++g_vaTotal;
      return;
    }
  }
}

void VaRemoveNoLock(uintptr_t base);
void VaRemove(uintptr_t base) {
  SpinLock lk;
  VaRemoveNoLock(base);
}
void VaRemoveNoLock(uintptr_t base) {
  size_t i = VaHash(base);
  for (size_t n = 0; n < kVaSlots; ++n, i = (i + 1) & (kVaSlots - 1)) {
    if (g_va[i].base == 0) return;
    if (g_va[i].base == base) {
      g_va[i].base = 1;
      --g_vaLive;
      return;
    }
  }
}

VaEntry* VaFindNoLock(uintptr_t base) {
  size_t i = VaHash(base);
  for (size_t n = 0; n < kVaSlots; ++n, i = (i + 1) & (kVaSlots - 1)) {
    if (g_va[i].base == 0) return nullptr;
    if (g_va[i].base == base) return &g_va[i];
  }
  return nullptr;
}

uintptr_t VaCallerNoLock(uintptr_t base) {
  size_t i = VaHash(base);
  for (size_t n = 0; n < kVaSlots; ++n, i = (i + 1) & (kVaSlots - 1)) {
    if (g_va[i].base == 0) return 0;
    if (g_va[i].base == base) return g_va[i].caller;
  }
  return 0;
}

uintptr_t VaCaller(uintptr_t base) {
  SpinLock lk;
  size_t i = VaHash(base);
  for (size_t n = 0; n < kVaSlots; ++n, i = (i + 1) & (kVaSlots - 1)) {
    if (g_va[i].base == 0) return 0;
    if (g_va[i].base == base) return g_va[i].caller;
  }
  return 0;
}

using VirtualAlloc_t = LPVOID(WINAPI*)(LPVOID, SIZE_T, DWORD, DWORD);
using VirtualFree_t = BOOL(WINAPI*)(LPVOID, SIZE_T, DWORD);
VirtualAlloc_t oVirtualAlloc = nullptr;
VirtualFree_t oVirtualFree = nullptr;

// #26 research (slots after a restart): the exe's first VirtualAlloc results, to compare two runs; and, with
// Binaries\Win32\DmCPT_fixedva.txt at start, its new reservations are placed one after another from a fixed address
// (a hint; the OS's choice when it is taken) - do the same requests then get the same addresses?
struct VaLogRec {
  uintptr_t addr, size, caller;
  DWORD type, tick;
};
constexpr int kVaLogMax = 8192;
VaLogRec g_valog[kVaLogMax];
std::atomic<int> g_valogN{0};
uintptr_t g_exeLo = 0, g_exeHi = 0;  // = g_exeBase/End, set before the hooks
bool g_fixedVa = false;
volatile LONG g_arenaLock = 0;
uintptr_t g_arenaNext = 0x90000000, g_arenaEnd = 0xE0000000;
std::atomic<long> g_arenaHits{0}, g_arenaMiss{0};

LPVOID WINAPI hkVirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD protect) {
  uintptr_t ret = (uintptr_t)_ReturnAddress();
  bool fromExe = ret >= g_exeLo && ret < g_exeHi;
  LPVOID r = nullptr;
  if (g_fixedVa && fromExe && !addr && (type & (MEM_RESERVE | MEM_COMMIT)) && !(type & MEM_TOP_DOWN)) {
    // (UE3's FMallocWindows asks MEM_COMMIT alone: at a given address that needs MEM_RESERVE too)
    while (InterlockedCompareExchange(&g_arenaLock, 1, 0) != 0) YieldProcessor();
    uintptr_t sz = (size + 0xFFFF) & ~(uintptr_t)0xFFFF;
    if (g_arenaNext + sz <= g_arenaEnd) {
      r = oVirtualAlloc((void*)g_arenaNext, size, type | MEM_RESERVE, protect);
      if (r) g_arenaNext += sz;
    }
    InterlockedExchange(&g_arenaLock, 0);
    ++(r ? g_arenaHits : g_arenaMiss);
  }
  if (!r) r = oVirtualAlloc(addr, size, type, protect);
  if (r && fromExe && !addr) {
    int i = g_valogN.fetch_add(1);
    if (i < kVaLogMax) g_valog[i] = {(uintptr_t)r, size, ret, type, GetTickCount()};
  }
  if (r) {
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery(r, &mbi, sizeof mbi)) VaInsert((uintptr_t)mbi.AllocationBase, (uintptr_t)_ReturnAddress());
  }
  return r;
}

// Quarantine (save states): while a slot holds an allocation of the game, the game's release of it only decommits
// the pages and keeps the address reserved, so a load can take it back (else the OS hands the address to the driver's
// heap and a load would overwrite that).
std::atomic<bool> g_quarantine{false};
std::atomic<long> g_zombies{0};
bool InAnySlotNoLock(uintptr_t base);  // below, with the slots (under g_vaLock)

BOOL WINAPI hkVirtualFree(LPVOID addr, SIZE_T size, DWORD type) {
  if ((type & MEM_RELEASE) && g_quarantine.load(std::memory_order_relaxed)) {
    SpinLock lk;
    VaEntry* e = VaFindNoLock((uintptr_t)addr);
    if (e && !e->zombie && InAnySlotNoLock((uintptr_t)addr) && oVirtualFree(addr, 0, MEM_DECOMMIT)) {
      e->zombie = 1;
      ++g_zombies;
      return TRUE;
    }
  }
  BOOL r = oVirtualFree(addr, size, type);
  if (r && (type & MEM_RELEASE)) VaRemove((uintptr_t)addr);
  return r;
}

// #30 (v1.0: "memory at 0BA70000 is used by something else"): a heap's big blocks and segments are its own
// NtAllocateVirtualMemory regions, released by ntdll itself (RtlFreeHeap -> NtFreeVirtualMemory), past the
// KernelBase!VirtualFree hook. A slot that holds such a block (AllocRec kind 2) needs its address kept: while it does,
// the heap's release only decommits and the address stays reserved ("heap zombie") until a load takes it back or no
// slot needs it. Only calls made from inside ntdll are touched (the heap's); VirtualFree callers - the game's, ours -
// pass as they are. No allocation and no g_vaLock here (the heap's lock is held; our frees run under g_vaLock).
using NtFreeVM_t = LONG(NTAPI*)(HANDLE, PVOID*, PSIZE_T, ULONG);
NtFreeVM_t oNtFreeVM = nullptr;
uintptr_t g_ntdllLo = 0, g_ntdllHi = 0;
volatile LONG g_hzLock = 0;
constexpr int kHzMax = 8192;
uintptr_t g_hzKnown[kHzMax];  // sorted: heap allocation bases of the valid slots (UpdateQuarantine)
int g_hzKnownN = 0;
uintptr_t g_hzZombie[kHzMax];  // heap regions released by the heap while a slot needed them (kept reserved)
int g_hzZombieN = 0;
std::atomic<long> g_hzTaken{0}, g_hzBack{0}, g_hzFreed{0}, g_hzCalls{0}, g_hzNtdll{0};
struct HzSpin {
  HzSpin() {
    while (InterlockedCompareExchange(&g_hzLock, 1, 0) != 0) YieldProcessor();
  }
  ~HzSpin() { InterlockedExchange(&g_hzLock, 0); }
};

bool HzKnownNoLock(uintptr_t b) { return std::binary_search(g_hzKnown, g_hzKnown + g_hzKnownN, b); }
bool HzIsZombieNoLock(uintptr_t b) {
  for (int i = 0; i < g_hzZombieN; ++i)
    if (g_hzZombie[i] == b) return true;
  return false;
}
// While the game is paused a frozen thread may hold g_hzLock (inside hkNtFreeVM): never wait for it then.
bool HzTryLock(int tries) {
  for (int i = 0; i < tries; ++i) {
    if (InterlockedCompareExchange(&g_hzLock, 1, 0) == 0) return true;
    YieldProcessor();
  }
  return false;
}
void HzUnlock() { InterlockedExchange(&g_hzLock, 0); }

LONG NTAPI hkNtFreeVM(HANDLE proc, PVOID* base, PSIZE_T size, ULONG type) {
  uintptr_t ret = (uintptr_t)_ReturnAddress();
  if (type & MEM_RELEASE) {
    ++g_hzCalls;
    if (ret >= g_ntdllLo && ret < g_ntdllHi) ++g_hzNtdll;
  }
  if ((type & MEM_RELEASE) && g_quarantine.load(std::memory_order_relaxed) && ret >= g_ntdllLo && ret < g_ntdllHi &&
      proc == (HANDLE)-1 && base && *base) {
    uintptr_t b = (uintptr_t)*base;
    HzSpin lk;
    if (HzKnownNoLock(b) && g_hzZombieN < kHzMax) {
      PVOID bb = *base;
      SIZE_T sz = 0;
      if (oNtFreeVM(proc, &bb, &sz, MEM_DECOMMIT) >= 0) {  // the whole region (base + size 0)
        g_hzZombie[g_hzZombieN++] = b;
        ++g_hzTaken;
        return 0;  // STATUS_SUCCESS: for the heap the block is gone
      }
    }
  }
  return oNtFreeVM(proc, base, size, type);
}

// TLS slots by owner: KernelBase!TlsAlloc hooked from DllMain. PhysX/APEX/UE3 keep per-thread pointers in TLS
// (the thread's TEB, not restored): a load puts the game's slots of every game thread back.
constexpr int kTlsMax = 1088;  // 64 + 1024 expansion slots
uintptr_t g_tlsOwner[kTlsMax];  // return address of the TlsAlloc call
using TlsAlloc_t = DWORD(WINAPI*)();
TlsAlloc_t oTlsAlloc = nullptr;
DWORD WINAPI hkTlsAlloc() {
  DWORD i = oTlsAlloc();
  if (i < (DWORD)kTlsMax) g_tlsOwner[i] = (uintptr_t)_ReturnAddress();
  return i;
}

// ---------------------------------------------------------------------------------------------------------------
// Modules and ntdll exports (where a suspended thread sits).
struct Module {
  uintptr_t base, end;
  std::string name;
};

std::vector<Module> Modules() {
  std::vector<Module> out;
  HMODULE mods[1024];
  DWORD need = 0;
  if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return out;
  for (DWORD i = 0; i < need / sizeof(HMODULE) && i < 1024; ++i) {
    MODULEINFO mi{};
    GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi);
    char path[MAX_PATH] = {};
    GetModuleFileNameA(mods[i], path, MAX_PATH);
    const char* s = strrchr(path, '\\');
    out.push_back({(uintptr_t)mi.lpBaseOfDll, (uintptr_t)mi.lpBaseOfDll + mi.SizeOfImage, s ? s + 1 : path});
  }
  std::sort(out.begin(), out.end(), [](const Module& a, const Module& b) { return a.base < b.base; });
  return out;
}

const Module* ModuleAt(const std::vector<Module>& mods, uintptr_t a) {
  auto it = std::upper_bound(mods.begin(), mods.end(), a, [](uintptr_t v, const Module& m) { return v < m.base; });
  if (it == mods.begin()) return nullptr;
  --it;
  return a < it->end ? &*it : nullptr;
}

std::string Where(const std::vector<Module>& mods, uintptr_t a) {
  char b[96];
  if (const Module* m = ModuleAt(mods, a))
    snprintf(b, sizeof b, "%s+0x%X", m->name.c_str(), (unsigned)(a - m->base));
  else
    snprintf(b, sizeof b, "%08X", (unsigned)a);
  return b;
}

struct Export {
  uintptr_t addr;
  const char* name;
};

std::vector<Export> Exports(HMODULE m) {
  std::vector<Export> out;
  auto base = (uint8_t*)m;
  auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
  auto& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if (!d.VirtualAddress) return out;
  auto ex = (IMAGE_EXPORT_DIRECTORY*)(base + d.VirtualAddress);
  auto names = (DWORD*)(base + ex->AddressOfNames);
  auto ords = (WORD*)(base + ex->AddressOfNameOrdinals);
  auto funcs = (DWORD*)(base + ex->AddressOfFunctions);
  for (DWORD i = 0; i < ex->NumberOfNames; ++i)
    out.push_back({(uintptr_t)(base + funcs[ords[i]]), (const char*)(base + names[i])});
  std::sort(out.begin(), out.end(), [](const Export& a, const Export& b) { return a.addr < b.addr; });
  return out;
}

std::string ExportAt(const std::vector<Export>& ex, uintptr_t a) {
  auto it = std::upper_bound(ex.begin(), ex.end(), a, [](uintptr_t v, const Export& e) { return v < e.addr; });
  if (it == ex.begin()) return "?";
  --it;
  if (a - it->addr > 0x400) return "?";
  return it->name;
}

using NtQIT_t = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
NtQIT_t NtQIT() {
  static auto f = (NtQIT_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread");
  return f;
}

struct ThreadInfo {
  DWORD tid = 0;
  uintptr_t start = 0, teb = 0, stackBase = 0, stackLimit = 0, stackAlloc = 0;
  uint64_t created = 0;  // creation time (a thread id can be reused)
  CONTEXT ctx{};
  bool ctxOk = false;
};

std::vector<DWORD> ThreadIds() {
  std::vector<DWORD> out;
  HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (s == INVALID_HANDLE_VALUE) return out;
  THREADENTRY32 te{sizeof te};
  DWORD pid = GetCurrentProcessId();
  for (BOOL ok = Thread32First(s, &te); ok; ok = Thread32Next(s, &te))
    if (te.th32OwnerProcessID == pid) out.push_back(te.th32ThreadID);
  CloseHandle(s);
  return out;
}

ThreadInfo Inspect(DWORD tid, bool context) {
  ThreadInfo t;
  t.tid = tid;
  HANDLE h = OpenThread(THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tid);
  if (!h) return t;
  NtQIT()(h, 9 /*ThreadQuerySetWin32StartAddress*/, &t.start, sizeof t.start, nullptr);
  FILETIME ct{}, et{}, kt{}, ut{};
  if (GetThreadTimes(h, &ct, &et, &kt, &ut)) t.created = ((uint64_t)ct.dwHighDateTime << 32) | ct.dwLowDateTime;
  struct {
    LONG status;
    PVOID teb;
    ULONG_PTR pid, tid;
    ULONG_PTR affinity;
    LONG prio, basePrio;
  } tbi{};
  if (NtQIT()(h, 0 /*ThreadBasicInformation*/, &tbi, sizeof tbi, nullptr) >= 0 && tbi.teb) {
    t.teb = (uintptr_t)tbi.teb;
    t.stackBase = util::ReadOr<uintptr_t>(t.teb + 4);
    t.stackLimit = util::ReadOr<uintptr_t>(t.teb + 8);
    t.stackAlloc = util::ReadOr<uintptr_t>(t.teb + 0xE0C);  // DeallocationStack (32-bit TEB)
  }
  if (context && tid != GetCurrentThreadId() && SuspendThread(h) != (DWORD)-1) {
    t.ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    t.ctxOk = GetThreadContext(h, &t.ctx) != 0;
    ResumeThread(h);
  }
  CloseHandle(h);
  return t;
}

bool CallBefore(uintptr_t ret, uintptr_t* target, const char** kind) {
  uint8_t b[7];
  if (!util::SafeRead(ret - 7, b, 7)) return false;
  if (b[2] == 0xE8) {  // call rel32
    *target = ret + *(int32_t*)(b + 3);
    *kind = "call";
    return true;
  }
  if (b[1] == 0xFF && ((b[2] >> 3) & 7) == 2 && (b[2] & 0xC7) == 0x05) {  // call [abs32]
    *target = util::ReadOr<uintptr_t>(*(uint32_t*)(b + 3));
    *kind = "call [mem]";
    return true;
  }
  for (int len = 2; len <= 6; ++len) {  // call r/m32 with a register / displacement
    const uint8_t* p = b + 7 - len;
    if (p[0] == 0xFF && ((p[1] >> 3) & 7) == 2) {
      *target = 0;
      *kind = "call indirect";
      return true;
    }
  }
  return false;
}

// Process heaps (and which is msvcr100's).
std::vector<uintptr_t> Heaps(uintptr_t* crtHeap) {
  HANDLE hs[256];
  DWORD n = GetProcessHeaps(256, hs);
  std::vector<uintptr_t> out;
  for (DWORD i = 0; i < n && i < 256; ++i) out.push_back((uintptr_t)hs[i]);
  *crtHeap = 0;
  if (HMODULE crt = GetModuleHandleW(L"msvcr100.dll"))
    if (auto f = (intptr_t(__cdecl*)())GetProcAddress(crt, "_get_heap_handle")) *crtHeap = (uintptr_t)f();
  return out;
}

bool Writable(DWORD p) {
  p &= 0xFF;
  return p == PAGE_READWRITE || p == PAGE_WRITECOPY || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

std::string MB(unsigned long long b) {
  char s[32];
  snprintf(s, sizeof s, "%.1f MB", b / 1048576.0);
  return s;
}

// ---------------------------------------------------------------------------------------------------------------
// Research: heap blocks by caller (only with Binaries\Win32\DmCPT_heaptrack.txt at start). ntdll Rtl*Heap hooked;
// a fixed table of live blocks (linear probing with backward-shift delete), no heap of its own.
struct HeapRec {
  uintptr_t key, size, heap, c1, c2;  // c1 = return address, c2 = first exe address further up the stack
};
constexpr size_t kHtSlots = 1 << 21;
HeapRec* g_ht = nullptr;
volatile LONG g_htLock = 0;
std::atomic<long> g_htLive{0}, g_htDrop{0};
uintptr_t g_exeBase = 0, g_exeEnd = 0;

struct Spin {
  volatile LONG* l;
  explicit Spin(volatile LONG* x) : l(x) {
    while (InterlockedCompareExchange(l, 1, 0) != 0) YieldProcessor();
  }
  ~Spin() { InterlockedExchange(l, 0); }
};

size_t HtHash(uintptr_t k) { return ((k >> 3) * 2654435761u) & (kHtSlots - 1); }

void HtInsert(uintptr_t key, uintptr_t size, uintptr_t heap, uintptr_t c1, uintptr_t c2) {
  Spin lk(&g_htLock);
  size_t i = HtHash(key);
  for (size_t n = 0; n < kHtSlots / 2; ++n, i = (i + 1) & (kHtSlots - 1)) {
    if (g_ht[i].key == key || g_ht[i].key == 0) {
      if (g_ht[i].key == 0) ++g_htLive;
      g_ht[i] = {key, size, heap, c1, c2};
      return;
    }
  }
  ++g_htDrop;
}

void HtRemove(uintptr_t key) {
  Spin lk(&g_htLock);
  size_t i = HtHash(key);
  for (size_t n = 0; n < kHtSlots / 2; ++n, i = (i + 1) & (kHtSlots - 1)) {
    if (g_ht[i].key == 0) return;
    if (g_ht[i].key != key) continue;
    --g_htLive;
    size_t j = i;  // backward-shift delete
    for (;;) {
      j = (j + 1) & (kHtSlots - 1);
      if (g_ht[j].key == 0) break;
      size_t k = HtHash(g_ht[j].key);
      bool move = (j > i) ? (k <= i || k > j) : (k <= i && k > j);
      if (move) {
        g_ht[i] = g_ht[j];
        i = j;
      }
    }
    g_ht[i].key = 0;
    return;
  }
}

uintptr_t ExeCallerAbove(void* retSlot) {
  uintptr_t* sp = (uintptr_t*)retSlot;
  uintptr_t top = __readfsdword(4);  // TEB StackBase
  for (int i = 1; i < 48 && (uintptr_t)(sp + i) + 4 <= top; ++i)
    if (sp[i] >= g_exeBase && sp[i] < g_exeEnd) return sp[i];
  return 0;
}

using RtlAlloc_t = PVOID(NTAPI*)(PVOID, ULONG, SIZE_T);
using RtlFree_t = BOOLEAN(NTAPI*)(PVOID, ULONG, PVOID);
using RtlReAlloc_t = PVOID(NTAPI*)(PVOID, ULONG, PVOID, SIZE_T);
RtlAlloc_t oRtlAlloc = nullptr;
RtlFree_t oRtlFree = nullptr;
RtlReAlloc_t oRtlReAlloc = nullptr;

PVOID NTAPI hkRtlAlloc(PVOID heap, ULONG flags, SIZE_T size) {
  PVOID r = oRtlAlloc(heap, flags, size);
  if (r) {
    uintptr_t c1 = (uintptr_t)_ReturnAddress();
    HtInsert((uintptr_t)r, size, (uintptr_t)heap, c1,
             (c1 >= g_exeBase && c1 < g_exeEnd) ? c1 : ExeCallerAbove(_AddressOfReturnAddress()));
  }
  return r;
}

BOOLEAN NTAPI hkRtlFree(PVOID heap, ULONG flags, PVOID p) {
  if (p) HtRemove((uintptr_t)p);
  return oRtlFree(heap, flags, p);
}

PVOID NTAPI hkRtlReAlloc(PVOID heap, ULONG flags, PVOID p, SIZE_T size) {
  PVOID r = oRtlReAlloc(heap, flags, p, size);
  if (r) {
    if (p && r != p) HtRemove((uintptr_t)p);
    uintptr_t c1 = (uintptr_t)_ReturnAddress();
    HtInsert((uintptr_t)r, size, (uintptr_t)heap, c1,
             (c1 >= g_exeBase && c1 < g_exeEnd) ? c1 : ExeCallerAbove(_AddressOfReturnAddress()));
  }
  return r;
}

// ---------------------------------------------------------------------------------------------------------------
// The game's clock. After a load the restored engine expects the time of the save: the exe's
// QueryPerformanceCounter (its import table entry only) is shifted back by the time between save and load.
using QPC_t = BOOL(WINAPI*)(LARGE_INTEGER*);
QPC_t oQPC = nullptr;
std::atomic<long long> g_qpcOff{0};

BOOL WINAPI hkQPC(LARGE_INTEGER* p) {
  BOOL r = oQPC(p);
  if (r && p) p->QuadPart -= g_qpcOff.load(std::memory_order_relaxed);
  return r;
}

long long RealQpc() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);  // the DLL's own import: not shifted
  return t.QuadPart;
}

double QpcMs(long long a, long long b) {
  static long long f = [] {
    LARGE_INTEGER x;
    QueryPerformanceFrequency(&x);
    return x.QuadPart;
  }();
  return (b - a) * 1000.0 / f;
}

void* PatchImport(uintptr_t module, const char* dll, const char* func, void* detour);
void* PatchExeImport(const char* dll, const char* func, void* detour) {
  return PatchImport((uintptr_t)GetModuleHandleW(nullptr), dll, func, detour);
}
// The import table entry of func (from dll) in a loaded module -> detour; returns the old value (nullptr: not
// imported, or already the detour).
void* PatchImport(uintptr_t module, const char* dll, const char* func, void* detour) {
  auto base = reinterpret_cast<uint8_t*>(module);
  auto nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
  const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
  if (!dir.VirtualAddress) return nullptr;
  for (auto imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); imp->Name; ++imp) {
    if (_stricmp(reinterpret_cast<const char*>(base + imp->Name), dll) != 0 || !imp->OriginalFirstThunk) continue;
    auto names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->OriginalFirstThunk);
    auto addrs = reinterpret_cast<IMAGE_THUNK_DATA*>(base + imp->FirstThunk);
    for (; names->u1.AddressOfData; ++names, ++addrs) {
      if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
      auto byName = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
      if (strcmp(reinterpret_cast<const char*>(byName->Name), func) != 0) continue;
      DWORD old = 0;
      if (reinterpret_cast<void*>(addrs->u1.Function) == detour) return nullptr;
      if (!VirtualProtect(&addrs->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return nullptr;
      void* orig = reinterpret_cast<void*>(addrs->u1.Function);
      addrs->u1.Function = reinterpret_cast<ULONG_PTR>(detour);
      VirtualProtect(&addrs->u1.Function, sizeof(void*), old, &old);
      return orig;
    }
  }
  return nullptr;
}

// ---------------------------------------------------------------------------------------------------------------
// The game's files (#26). Restored memory holds kernel file handles: UE3's package readers and async IO cache, Wwise's
// streams. A part of the level that unloads after a save closes its files; after a load the restored memory reads
// them again (a closed handle, or worse, a reused value). The exe's CreateFileW / CloseHandle imports are patched:
// read-only files it opens are tracked; while a slot knows a handle (it was open at that save), the game's close only
// marks it closed ("zombie") and the handle stays open. A save remembers each open handle's file position (UE3's
// synchronous readers rely on it); a load makes the slot's handles live again at their positions and really closes
// the ones no slot knows (opened after the save: the restored memory has no trace of them).
struct FileRec {
  HANDLE h;
  uint8_t used;    // 0 empty, 1 used, 2 deleted
  uint8_t mask;    // slots that saved it open
  uint8_t zombie;  // the game closed it, a slot still needs it
  LONGLONG pos[kSlots];
};
constexpr size_t kFileSlots = 8192;
FileRec g_files[kFileSlots];
volatile LONG g_fileLock = 0;
std::atomic<long> g_fileLive{0}, g_fileZombies{0}, g_fileTracked{0}, g_fileClosedAtLoad{0};
std::atomic<bool> g_armed{false};

using CreateFileW_t = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using CloseHandle_t = BOOL(WINAPI*)(HANDLE);
CreateFileW_t oCreateFileW = nullptr;
CloseHandle_t oCloseHandle = nullptr;

size_t FileHash(HANDLE h) { return (((uintptr_t)h >> 2) * 2654435761u) & (kFileSlots - 1); }

FileRec* FileFindNoLock(HANDLE h) {
  size_t i = FileHash(h);
  for (size_t n = 0; n < kFileSlots; ++n, i = (i + 1) & (kFileSlots - 1)) {
    if (g_files[i].used == 0) return nullptr;
    if (g_files[i].used == 1 && g_files[i].h == h) return &g_files[i];
  }
  return nullptr;
}

void FileInsertNoLock(HANDLE h) {
  size_t i = FileHash(h), tomb = SIZE_MAX;
  for (size_t n = 0; n < kFileSlots; ++n, i = (i + 1) & (kFileSlots - 1)) {
    if (g_files[i].used == 1 && g_files[i].h == h) return;
    if (g_files[i].used == 2 && tomb == SIZE_MAX) tomb = i;
    if (g_files[i].used == 0) {
      size_t at = tomb != SIZE_MAX ? tomb : i;
      g_files[at] = {h, 1, 0, 0, {}};
      ++g_fileLive;
      ++g_fileTracked;
      return;
    }
  }
}

void FileRemoveNoLock(FileRec* r) {
  r->used = 2;
  r->h = nullptr;
  --g_fileLive;
}

HANDLE WINAPI hkCreateFileW(LPCWSTR name, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disp, DWORD flags,
                            HANDLE tmpl) {
  HANDLE r = oCreateFileW(name, access, share, sa, disp, flags, tmpl);
  constexpr DWORD kWrite = GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA | DELETE | WRITE_DAC |
                           WRITE_OWNER | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA;
  if (r != INVALID_HANDLE_VALUE && g_armed.load(std::memory_order_relaxed) && !(access & kWrite) &&
      disp == OPEN_EXISTING && name && !(name[0] == L'\\' && name[1] == L'\\' && name[2] == L'.')) {
    Spin lk(&g_fileLock);
    FileInsertNoLock(r);
  }
  return r;
}

std::atomic<long> g_hrecLive{0};  // #28: tracked kernel objects (see "Kernel objects of the save")
bool HeldCloseNoLock(HANDLE h);

BOOL WINAPI hkCloseHandle(HANDLE h) {
  if (g_fileLive.load(std::memory_order_relaxed) > 0 || g_hrecLive.load(std::memory_order_relaxed) > 0) {
    Spin lk(&g_fileLock);
    if (FileRec* r = FileFindNoLock(h)) {
      if (r->zombie) return TRUE;  // closed twice by the game: already closed for it
      if (r->mask) {               // a slot needs it: stays open
        r->zombie = 1;
        ++g_fileZombies;
        return TRUE;
      }
      FileRemoveNoLock(r);
    } else if (HeldCloseNoLock(h)) {
      return TRUE;
    }
  }
  return oCloseHandle(h);
}

LONGLONG FilePos(HANDLE h) {
  LARGE_INTEGER z{}, cur{};
  return SetFilePointerEx(h, z, &cur, FILE_CURRENT) ? cur.QuadPart : -1;
}

// Save of slot s (paused, g_fileLock held): open handles belong to the slot, closed ones not.
int FilesSaveNoLock(int s) {
  int n = 0;
  for (FileRec& r : g_files) {
    if (r.used != 1) continue;
    if (!r.zombie) {
      r.mask |= 1 << s;
      r.pos[s] = FilePos(r.h);
      ++n;
    } else {
      r.mask &= ~(1 << s);
      if (!r.mask) {
        oCloseHandle(r.h);
        --g_fileZombies;
        FileRemoveNoLock(&r);
      }
    }
  }
  return n;
}

// Load of slot s (paused, g_fileLock held). Returns the handles made live again; *closed = really closed.
int FilesLoadNoLock(int s, int* closed, int* moved) {
  int revived = 0;
  *closed = *moved = 0;
  for (FileRec& r : g_files) {
    if (r.used != 1) continue;
    if (r.mask & (1 << s)) {
      if (r.zombie) {
        r.zombie = 0;
        --g_fileZombies;
        ++revived;
      }
      if (r.pos[s] >= 0 && FilePos(r.h) != r.pos[s]) {
        LARGE_INTEGER p;
        p.QuadPart = r.pos[s];
        SetFilePointerEx(r.h, p, nullptr, FILE_BEGIN);
        ++*moved;
      }
    } else if (r.mask) {  // another slot knows it: closed for the game now, kept open
      if (!r.zombie) {
        r.zombie = 1;
        ++g_fileZombies;
      }
    } else {  // opened after the save, no slot knows it: the restored game has no trace of it
      oCloseHandle(r.h);
      if (r.zombie) --g_fileZombies;
      FileRemoveNoLock(&r);
      ++*closed;
    }
  }
  g_fileClosedAtLoad += *closed;
  return revived;
}

// Slot s is gone (g_fileLock held): handles only it kept, and closed by the game, really close.
void FilesDropSlotNoLock(int s) {
  for (FileRec& r : g_files) {
    if (r.used != 1) continue;
    r.mask &= ~(1 << s);
    if (!r.mask && r.zombie) {
      oCloseHandle(r.h);
      --g_fileZombies;
      FileRemoveNoLock(&r);
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------
// #28: kernel objects of the save. A map opened again (Restart, Load checkpoint, another mission) destroys the
// physics thread pools: Havok's semaphores, PhysX's events, the handles of their threads - closed by the exe / the
// game's dlls. The restored memory of an older slot names them again. Like the files: every event / semaphore /
// mutex / thread handle open at a save is known to that slot; while a slot knows it, the game's CloseHandle (imports
// of the exe and of every dll of the game's folder) only marks it closed ("zombie"), the handle stays open - its
// value is never reused. A load makes the slot's zombies live again with the state of the save (event set or not,
// semaphore count). The list of handles: NtQueryInformationProcess(ProcessHandleInformation), during the pause.
using NtQIP_t = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
NtQIP_t g_ntQip = nullptr;
using NtQuerySem_t = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
NtQuerySem_t g_ntQuerySem = nullptr;
using NtQueryEvent2_t = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
NtQueryEvent2_t g_ntQueryEv = nullptr;
ULONG g_tyEvent = ~0u, g_tySem = ~0u, g_tyMutex = ~0u, g_tyThread = ~0u;  // object type indices (this Windows)
enum : uint8_t { kObjEvent = 1, kObjSem = 2, kObjMutex = 3, kObjThread = 4 };

struct HEnt {
  HANDLE h;
  ULONG type;
};
uint8_t g_hbuf[4 << 20];  // the snapshot (static: also used while the game is frozen)
HEnt g_hents[65536];
int g_hentN = 0;
volatile LONG g_enumLock = 0;
LONG g_enumStatus = 0;

// Every handle of the process now -> g_hents (caller holds g_enumLock). No heap.
int EnumHandlesNoLock() {
  g_hentN = 0;
  if (!g_ntQip) return 0;
  ULONG len = 0;
  g_enumStatus = g_ntQip(GetCurrentProcess(), 51 /*ProcessHandleInformation*/, g_hbuf, sizeof g_hbuf, &len);
  if (g_enumStatus < 0) return 0;
  struct Entry {
    HANDLE HandleValue;
    ULONG_PTR HandleCount, PointerCount;
    ULONG GrantedAccess, ObjectTypeIndex, HandleAttributes, Reserved;
  };
  ULONG_PTR n = *(ULONG_PTR*)g_hbuf;
  auto e = (const Entry*)(g_hbuf + 2 * sizeof(ULONG_PTR));
  for (ULONG_PTR i = 0; i < n && g_hentN < (int)std::size(g_hents); ++i)
    g_hents[g_hentN++] = {e[i].HandleValue, e[i].ObjectTypeIndex};
  return g_hentN;
}

uint8_t ObjKind(ULONG ty) {
  return ty == g_tyEvent ? kObjEvent : ty == g_tySem ? kObjSem : ty == g_tyMutex ? kObjMutex : ty == g_tyThread ? kObjThread : 0;
}

// The type indices of this Windows: make one object of each kind and find it in the snapshot.
void FindObjectTypes() {
  HMODULE nt = GetModuleHandleW(L"ntdll.dll");
  g_ntQip = (NtQIP_t)GetProcAddress(nt, "NtQueryInformationProcess");
  g_ntQuerySem = (NtQuerySem_t)GetProcAddress(nt, "NtQuerySemaphore");
  g_ntQueryEv = (NtQueryEvent2_t)GetProcAddress(nt, "NtQueryEvent");
  HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr), sem = CreateSemaphoreW(nullptr, 0, 1, nullptr),
         mu = CreateMutexW(nullptr, FALSE, nullptr),
         th = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, GetCurrentThreadId());
  {
    Spin lk(&g_enumLock);
    EnumHandlesNoLock();
    for (int i = 0; i < g_hentN; ++i) {
      if (g_hents[i].h == ev) g_tyEvent = g_hents[i].type;
      if (g_hents[i].h == sem) g_tySem = g_hents[i].type;
      if (g_hents[i].h == mu) g_tyMutex = g_hents[i].type;
      if (g_hents[i].h == th) g_tyThread = g_hents[i].type;
    }
  }
  LOG("savestate: handle list %s (status %08X, %d handles); object types event %lu semaphore %lu mutex %lu thread %lu",
      g_hentN ? "OK" : "NOT available", (unsigned)g_enumStatus, g_hentN, g_tyEvent, g_tySem, g_tyMutex, g_tyThread);
  CloseHandle(ev);
  CloseHandle(sem);
  CloseHandle(mu);
  CloseHandle(th);
}

LONG ObjState(HANDLE h, uint8_t kind) {  // event: signalled 0/1; semaphore: count; -1 unknown
  struct {
    LONG a, b;
  } info{};
  if (kind == kObjEvent && g_ntQueryEv) return g_ntQueryEv(h, 0, &info, sizeof info, nullptr) >= 0 ? info.b : -1;
  if (kind == kObjSem && g_ntQuerySem) return g_ntQuerySem(h, 0, &info, sizeof info, nullptr) >= 0 ? info.a : -1;
  return -1;
}

struct HRec {
  HANDLE h;
  uint8_t used;  // 0 empty, 1 used, 2 deleted
  uint8_t kind;
  uint8_t mask;    // slots that saved it open
  uint8_t zombie;  // closed by the game, a slot still needs it
  LONG st[kSlots];  // state at each slot's save (-1 unknown)
};
constexpr size_t kHSlots = 16384;
HRec g_hrec[kHSlots];
std::atomic<long> g_hZombies{0}, g_hHeld{0}, g_hRevived{0}, g_hStateSet{0};

size_t HHash(HANDLE h) { return (((uintptr_t)h >> 2) * 2654435761u) & (kHSlots - 1); }
HRec* HFindNoLock(HANDLE h) {
  size_t i = HHash(h);
  for (size_t n = 0; n < kHSlots; ++n, i = (i + 1) & (kHSlots - 1)) {
    if (g_hrec[i].used == 0) return nullptr;
    if (g_hrec[i].used == 1 && g_hrec[i].h == h) return &g_hrec[i];
  }
  return nullptr;
}
HRec* HInsertNoLock(HANDLE h, uint8_t kind) {
  size_t i = HHash(h), tomb = SIZE_MAX;
  for (size_t n = 0; n < kHSlots; ++n, i = (i + 1) & (kHSlots - 1)) {
    if (g_hrec[i].used == 1 && g_hrec[i].h == h) return &g_hrec[i];
    if (g_hrec[i].used == 2 && tomb == SIZE_MAX) tomb = i;
    if (g_hrec[i].used == 0) {
      size_t at = tomb != SIZE_MAX ? tomb : i;
      g_hrec[at] = {h, 1, kind, 0, 0, {}};
      for (LONG& x : g_hrec[at].st) x = -1;
      ++g_hrecLive;
      return &g_hrec[at];
    }
  }
  return nullptr;
}
void HRemoveNoLock(HRec* r) {
  r->used = 2;
  r->h = nullptr;
  --g_hrecLive;
}

// hkCloseHandle (g_fileLock held): true = held back (a slot knows the object).
bool HeldCloseNoLock(HANDLE h) {
  HRec* r = HFindNoLock(h);
  if (!r) return false;
  if (r->zombie) return true;  // closed again by the game: closed for it already
  if (r->mask) {
    r->zombie = 1;
    ++g_hZombies;
    ++g_hHeld;
    return true;
  }
  HRemoveNoLock(r);
  return false;
}

bool OwnHandle(HANDLE h);  // a handle of the tool's pause list (below)
// Save of slot s (paused, g_fileLock + g_enumLock held, g_hents = the handles now).
int HandlesSaveNoLock(int s) {
  int n = 0;
  for (HRec& r : g_hrec)
    if (r.used == 1 && !r.zombie) r.mask &= ~(1 << s);  // re-marked below if still open
  for (int i = 0; i < g_hentN; ++i) {
    uint8_t k = ObjKind(g_hents[i].type);
    if (!k || OwnHandle(g_hents[i].h)) continue;  // the tool's own thread handles are not the game's
    HRec* r = HFindNoLock(g_hents[i].h);
    if (r && r->zombie) {  // closed for the game: not part of this save
      r->mask &= ~(1 << s);
      if (!r->mask) {
        oCloseHandle(r->h);
        --g_hZombies;
        HRemoveNoLock(r);
      }
      continue;
    }
    if (!r || r->kind != k) {
      if (r) HRemoveNoLock(r);
      r = HInsertNoLock(g_hents[i].h, k);
    }
    if (!r) continue;
    r->mask |= 1 << s;
    r->st[s] = ObjState(r->h, k);
    ++n;
  }
  for (HRec& r : g_hrec)  // known to no slot any more and not held: forget it
    if (r.used == 1 && !r.mask && !r.zombie) HRemoveNoLock(&r);
  return n;
}

// Load of slot s (paused, g_fileLock held): the slot's zombies are the game's again, with their state of the save.
int HandlesLoadNoLock(int s, int* states) {
  int revived = 0;
  *states = 0;
  for (HRec& r : g_hrec) {
    if (r.used != 1) continue;
    if (r.mask & (1 << s)) {
      if (!r.zombie) continue;
      r.zombie = 0;
      --g_hZombies;
      ++revived;
      LONG want = r.st[s], now = ObjState(r.h, r.kind);
      if (want < 0 || now < 0 || want == now) continue;
      if (r.kind == kObjEvent) {
        want ? SetEvent(r.h) : ResetEvent(r.h);
      } else if (r.kind == kObjSem) {
        if (want > now) ReleaseSemaphore(r.h, want - now, nullptr);
        else
          for (LONG k = now; k > want; --k) WaitForSingleObject(r.h, 0);
      }
      ++*states;
    }
    // (a handle made after this save stays as it is: the restored game does not know it and never closes it; only
    // closes we held back are ever closed by us - never a handle of someone else)
  }
  g_hRevived += revived;
  g_hStateSet += *states;
  return revived;
}

void HandlesDropSlotNoLock(int s) {
  for (HRec& r : g_hrec) {
    if (r.used != 1) continue;
    r.mask &= ~(1 << s);
    if (r.mask) continue;
    if (r.zombie) {
      oCloseHandle(r.h);
      --g_hZombies;
    }
    HRemoveNoLock(&r);
  }
}

// ---------------------------------------------------------------------------------------------------------------
// #28: kept threads. A game thread of a valid slot (tid + creation time) that ends - the game shuts its pool down when
// it opens a map again - parks instead (RtlExitUserThread hooked, see ss_ParkLoop): for the game it is gone (its
// handle reads as signalled, GetExitCodeThread gives its exit code), for us it waits, its stack, TEB, TLS and FLS
// intact. TerminateThread by the game (PhysX kills a thread that did not quit) on such a thread, if it waits in a
// system call: it is only frozen (suspended). A load of the slot: parked -> back into its wait of the save (stack,
// registers); frozen -> resumed where it waits. Threads of the dropped timeline: known to the other slot -> frozen;
// unknown -> ended (TerminateThread: each waits idle in a wait call, no lock held). The rendering thread is not kept
// (#27: Stop/StartRenderingThread). All under g_fileLock.
struct ThreadKey {
  DWORD tid;
  uint64_t created;
  uintptr_t start;
  bool operator<(const ThreadKey& o) const { return tid < o.tid; }
};
struct Known {
  DWORD tid;
  uint64_t created;
  uint8_t mask;
};
Known g_known[512];
int g_knownN = 0;
DWORD g_knownRender[kSlots] = {};

enum : long { kKeepFree = 0, kKeepParked = 1, kKeepFrozen = 2, kKeepReviving = 3, kKeepClaim = 4, kKeepReleasing = 5 };
struct KeepRec {
  volatile long state;
  DWORD tid;
  uint64_t created;
  uintptr_t start;
  HANDLE h;           // ours, all access
  bool byTerminate;   // frozen because the game asked TerminateThread
  DWORD exitCode;
  HANDLE hv[6];       // handle values in the process naming this thread (the game's)
  int nhv;
  uintptr_t priv;     // the private stack (kept for the next park)
  SsPark park;
  DWORD parkedAt;
};
constexpr int kKeepMax = 96;
constexpr size_t kPrivStack = 64 << 10;
KeepRec g_keep[kKeepMax];
std::atomic<int> g_keepLive{0};
std::atomic<long> g_parks{0}, g_revives{0}, g_freezes{0}, g_unfreezes{0}, g_kills{0}, g_termHeld{0}, g_released{0},
    g_waitsFaked{0};
// handles that read as signalled (ended threads for the game): copy for the wait hooks
// #30: was 128 - v1.0 quit hung with ~43 parked threads x 3 handles (the game waited on a handle left out of the table)
constexpr int kDeadMax = kKeepMax * 6;
HANDLE g_dead[kDeadMax];
DWORD g_deadCode[kDeadMax], g_deadTid[kDeadMax];
uint64_t g_deadCreated[kDeadMax];
std::atomic<int> g_deadN{0};
HANDLE g_signalled[64];  // manual-reset events, always set (stand-ins in WaitForMultipleObjects)

using WFSO_t = DWORD(WINAPI*)(HANDLE, DWORD);
using WFSOEx_t = DWORD(WINAPI*)(HANDLE, DWORD, BOOL);
using WFMO_t = DWORD(WINAPI*)(DWORD, const HANDLE*, BOOL, DWORD);
using WFMOEx_t = DWORD(WINAPI*)(DWORD, const HANDLE*, BOOL, DWORD, BOOL);
using GECT_t = BOOL(WINAPI*)(HANDLE, LPDWORD);
using TT_t = BOOL(WINAPI*)(HANDLE, DWORD);
using RtlExit_t = void(NTAPI*)(LONG);
WFSO_t oWFSO = nullptr;
WFSOEx_t oWFSOEx = nullptr;
WFMO_t oWFMO = nullptr;
WFMOEx_t oWFMOEx = nullptr;
GECT_t oGECT = nullptr;
TT_t oTerminateThread = nullptr;
RtlExit_t oRtlExitUserThread = nullptr;

uint64_t ThreadCreated(HANDLE h) {
  FILETIME ct{}, et{}, kt{}, ut{};
  return GetThreadTimes(h, &ct, &et, &kt, &ut) ? ((uint64_t)ct.dwHighDateTime << 32) | ct.dwLowDateTime : 0;
}

uint8_t KnownMaskNoLock(DWORD tid, uint64_t created) {
  if (tid == g_knownRender[0] || tid == g_knownRender[1]) return 0;
  for (int i = 0; i < g_knownN; ++i)
    if (g_known[i].tid == tid && g_known[i].created == created) return g_known[i].mask;
  return 0;
}

int KeepFindNoLock(DWORD tid, uint64_t created) {
  for (int i = 0; i < kKeepMax; ++i)
    if (g_keep[i].state != kKeepFree && g_keep[i].tid == tid && g_keep[i].created == created) return i;
  return -1;
}

void RebuildDeadNoLock() {
  int n = 0;
  for (KeepRec& k : g_keep)
    if (k.state == kKeepParked || (k.state == kKeepFrozen && k.byTerminate))
      for (int j = 0; j < k.nhv && n < (int)std::size(g_dead); ++j)
        g_dead[n] = k.hv[j], g_deadCode[n] = k.exitCode, g_deadTid[n] = k.tid, g_deadCreated[n] = k.created, ++n;
  g_deadN = n;
}

// Handle values (at a save) naming the threads the slots know: a wait on one of them is done in 20 ms slices - the
// thread may park (end for the game) while the game already waits on it in the kernel (PhysX waits for its thread
// to quit with INFINITE): each slice ends, the hook then sees it ended.
HANDLE g_watch[256];
DWORD g_watchTid[256];
uint64_t g_watchCreated[256];
std::atomic<int> g_watchN{0};

uint64_t ThreadCreated(HANDLE h);
// 2 = an ended thread for the game (*code = its exit code), 1 = a thread a slot knows, 0 = other. A value matches only
// if it still names that very thread (id + creation time): handle values are reused - the tool's own thread handles
// (closed after a pause) came back as the game's events, whose waits then "ended" at once (#28 crash, R0 #1).
int Classify(HANDLE h, DWORD* code) {
  if ((g_deadN.load(std::memory_order_relaxed) == 0 && g_watchN.load(std::memory_order_relaxed) == 0) || !h) return 0;
  // no lock (every wait of the game passes here while a slot exists; a global spin lock cost FPS): the tables change
  // rarely and a torn read is harmless - a match is checked against the thread itself below, a match seen late is
  // caught by the next 20 ms slice
  int kind = 0;
  DWORD tid = 0, c = 0;
  uint64_t created = 0;
  {
    int nd = g_deadN.load(std::memory_order_acquire), nw = g_watchN.load(std::memory_order_acquire);
    volatile HANDLE* dv = g_dead;
    volatile HANDLE* wv = g_watch;
    for (int i = 0; i < nd && !kind; ++i)
      if (dv[i] == h) kind = 2, c = g_deadCode[i], tid = g_deadTid[i], created = g_deadCreated[i];
    for (int i = 0; i < nw && !kind; ++i)
      if (wv[i] == h) kind = 1, tid = g_watchTid[i], created = g_watchCreated[i];
  }
  if (!kind || GetThreadId(h) != tid || ThreadCreated(h) != created) return 0;
  if (code) *code = c;
  return kind;
}
// #30: a handle of a parked (or game-terminated, frozen) thread that the tables do not list (opened after the park,
// or a full table): found by the thread itself. System calls - only where a wait already timed out once / GECT.
std::atomic<long> g_deadByTid{0};
bool ParkedExit(HANDLE h, DWORD* code) {
  if (g_keepLive.load(std::memory_order_relaxed) == 0 || !h) return false;
  DWORD tid = GetThreadId(h);
  if (!tid) return false;
  uint64_t created = 0;
  for (const KeepRec& k : g_keep) {
    long st = k.state;
    if (k.tid != tid || !(st == kKeepParked || (st == kKeepFrozen && k.byTerminate))) continue;
    if (!created) created = ThreadCreated(h);
    if (k.created != created) continue;
    if (code) *code = k.exitCode;
    ++g_deadByTid;
    return true;
  }
  return false;
}
bool DeadHandle(HANDLE h, DWORD* code) { return Classify(h, code) == 2 || ParkedExit(h, code); }

// #30 (v1.0 hang after Restart #13: PhysX waited INFINITE on its worker's handle 0x2128, opened after the save, so
// not among g_watch; the worker then parked - alive - and the wait never ended): a long wait on any handle of a
// thread a slot knows is sliced too. Two system calls per long wait while slots exist (a long wait blocks anyway).
std::atomic<long> g_lateWatch{0};
bool KnownThreadHandle(HANDLE h) {
  if (g_knownN == 0 || !h) return false;
  DWORD tid = GetThreadId(h);  // 0 for a handle that is not a thread
  if (!tid || tid == GetCurrentThreadId()) return false;
  uint64_t created = ThreadCreated(h);
  // no lock, as in Classify: the table changes rarely, a torn read only costs a plain wait or one more slice
  return KnownMaskNoLock(tid, created) != 0;
}

// The handle values naming thread tid (g_enumLock taken here).
int ThreadHandles(DWORD tid, HANDLE* out, int max, HANDLE also) {
  int n = 0;
  if (also) out[n++] = also;
  Spin lk(&g_enumLock);
  EnumHandlesNoLock();
  for (int i = 0; i < g_hentN && n < max; ++i)
    if (g_hents[i].type == g_tyThread && g_hents[i].h != also && GetThreadId(g_hents[i].h) == tid) out[n++] = g_hents[i].h;
  return n;
}

std::atomic<long> g_slicedWaits{0};
DWORD SlicedWait(HANDLE h, DWORD ms, BOOL alert) {
  ++g_slicedWaits;
  DWORD t0 = GetTickCount();
  for (;;) {
    DWORD left = ms == INFINITE ? 20 : ms - std::min<DWORD>(ms, GetTickCount() - t0);
    DWORD r = oWFSOEx(h, std::min<DWORD>(20, left), alert);
    if (r != WAIT_TIMEOUT) return r;
    if (DeadHandle(h, nullptr)) return ++g_waitsFaked, WAIT_OBJECT_0;
    if (ms != INFINITE && GetTickCount() - t0 >= ms) return WAIT_TIMEOUT;
  }
}
DWORD WINAPI hkWFSO(HANDLE h, DWORD ms) {
  int c = Classify(h, nullptr);
  if (c == 2) return ++g_waitsFaked, WAIT_OBJECT_0;
  if (ms > 20 && (c == 1 || (KnownThreadHandle(h) && ++g_lateWatch))) return SlicedWait(h, ms, FALSE);
  return oWFSO(h, ms);
}
DWORD WINAPI hkWFSOEx(HANDLE h, DWORD ms, BOOL alert) {
  int c = Classify(h, nullptr);
  if (c == 2) return ++g_waitsFaked, WAIT_OBJECT_0;
  if (ms > 20 && (c == 1 || (KnownThreadHandle(h) && ++g_lateWatch))) return SlicedWait(h, ms, alert);
  return oWFSOEx(h, ms, alert);
}
// A copy of the array with each ended thread replaced by a set event (distinct ones: no duplicates allowed).
const HANDLE* DeadSubst(DWORD n, const HANDLE* hs, HANDLE* tmp, bool* watched, bool deep = false) {
  *watched = false;
  if ((g_deadN.load(std::memory_order_relaxed) == 0 && g_watchN.load(std::memory_order_relaxed) == 0) || n > 64 || !hs)
    return hs;
  bool any = false;
  for (DWORD i = 0; i < n; ++i) {
    tmp[i] = hs[i];
    int c = Classify(hs[i], nullptr);
    if (c != 2 && deep && ParkedExit(hs[i], nullptr)) c = 2;  // #30: in the sliced loop only
    if (c == 2 && g_signalled[i]) tmp[i] = g_signalled[i], any = true;
    if (c == 1) *watched = true;
  }
  if (any) ++g_waitsFaked;
  return any ? tmp : hs;
}
DWORD WaitMulti(DWORD n, const HANDLE* hs, BOOL all, DWORD ms, BOOL alert) {
  HANDLE tmp[64];
  bool watched = false;
  const HANDLE* use = DeadSubst(n, hs, tmp, &watched);
  if (!watched && ms > 20 && hs && n <= 64)  // #30: a thread handle opened after the save (see KnownThreadHandle)
    for (DWORD i = 0; i < n && !watched; ++i)
      if (KnownThreadHandle(hs[i])) watched = true, ++g_lateWatch;
  if (!watched || ms <= 20) return oWFMOEx(n, use, all, ms, alert);
  ++g_slicedWaits;
  DWORD t0 = GetTickCount();
  for (;;) {
    DWORD left = ms == INFINITE ? 20 : ms - std::min<DWORD>(ms, GetTickCount() - t0);
    DWORD r = oWFMOEx(n, use, all, std::min<DWORD>(20, left), alert);
    if (r != WAIT_TIMEOUT) return r;
    if (ms != INFINITE && GetTickCount() - t0 >= ms) return WAIT_TIMEOUT;
    use = DeadSubst(n, hs, tmp, &watched, true);  // a thread may have ended for the game meanwhile
  }
}
DWORD WINAPI hkWFMO(DWORD n, const HANDLE* hs, BOOL all, DWORD ms) { return WaitMulti(n, hs, all, ms, FALSE); }
DWORD WINAPI hkWFMOEx(DWORD n, const HANDLE* hs, BOOL all, DWORD ms, BOOL alert) {
  return WaitMulti(n, hs, all, ms, alert);
}
BOOL WINAPI hkGECT(HANDLE h, LPDWORD code) {
  DWORD c = 0;
  if (DeadHandle(h, &c)) {
    if (code) *code = c;
    return TRUE;
  }
  return oGECT(h, code);
}

bool InWaitStub(uintptr_t eip);
std::string Fmt(const char* f, ...);
std::string NameOf(DWORD tid);

BOOL WINAPI hkTerminateThread(HANDLE h, DWORD code) {
  if (g_armed && g_knownN > 0 && h && h != GetCurrentThread()) {
    DWORD tid = GetThreadId(h);
    uint64_t created = ThreadCreated(h);
    uint8_t m;
    {
      Spin lk(&g_fileLock);
      m = KnownMaskNoLock(tid, created);
    }
    if (m && tid != GetCurrentThreadId() && SuspendThread(h) != (DWORD)-1) {
      CONTEXT c{};
      c.ContextFlags = CONTEXT_CONTROL;
      bool waiting = GetThreadContext(h, &c) && InWaitStub(c.Eip);
      HANDLE own = waiting ? OpenThread(THREAD_ALL_ACCESS, FALSE, tid) : nullptr;
      if (own) {
        HANDLE hv[6];
        int nhv = ThreadHandles(tid, hv, 6, h);
        Spin lk(&g_fileLock);
        for (KeepRec& k : g_keep)
          if (k.state == kKeepFree) {
            k.tid = tid, k.created = created, k.start = 0, k.h = own, k.byTerminate = true, k.exitCode = code;
            k.nhv = nhv;
            memcpy(k.hv, hv, sizeof hv);
            k.parkedAt = GetTickCount();
            k.state = kKeepFrozen;
            ++g_keepLive;
            ++g_termHeld;
            RebuildDeadNoLock();
            own = nullptr;
            break;
          }
      }
      if (!own && waiting) {
        LOG("savestate: TerminateThread(%lu %s) by the game held back: frozen in its wait (a slot knows it)", tid,
            NameOf(tid).c_str());
        return TRUE;
      }
      if (own) CloseHandle(own);
      ResumeThread(h);
      LOG("savestate: TerminateThread(%lu %s) by the game: not waiting (or no room) - it ends; slots that know it "
          "cannot load",
          tid, NameOf(tid).c_str());
    }
  }
  return oTerminateThread(h, code);
}

void NTAPI hkRtlExitUserThread(LONG status) {
  if (g_armed.load(std::memory_order_relaxed) && g_knownN > 0) {
    DWORD tid = GetCurrentThreadId();
    uint64_t created = ThreadCreated(GetCurrentThread());
    KeepRec* r = nullptr;
    {
      Spin lk(&g_fileLock);
      if (KnownMaskNoLock(tid, created))
        for (KeepRec& k : g_keep)
          if (k.state == kKeepFree) {
            k.state = kKeepClaim;
            r = &k;
            break;
          }
    }
    if (r) {
      if (!r->priv) r->priv = (uintptr_t)oVirtualAlloc(nullptr, kPrivStack, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
      if (!r->park.ev) r->park.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      HANDLE own = OpenThread(THREAD_ALL_ACCESS, FALSE, tid);
      if (r->priv && r->park.ev && own) {
        r->tid = tid, r->created = created, r->h = own, r->byTerminate = false, r->exitCode = (DWORD)status;
        r->start = 0;
        r->park.cmd = 0;
        r->nhv = ThreadHandles(tid, r->hv, 6, nullptr);
        r->parkedAt = GetTickCount();
        {
          Spin lk(&g_fileLock);
          r->state = kKeepParked;
          ++g_keepLive;
          ++g_parks;
          RebuildDeadNoLock();
        }
        LOG("savestate: thread %lu %s ended by the game - kept (a slot knows it), %d handle(s)", tid,
            NameOf(tid).c_str(), r->nhv);
        ss_ParkOnStack(r->priv + kPrivStack - 64, &r->park);
        // released (no slot needs it): end for real
        {
          Spin lk(&g_fileLock);
          r->state = kKeepFree;
          --g_keepLive;
          RebuildDeadNoLock();
        }
        CloseHandle(own);
      } else {
        if (own) CloseHandle(own);
        Spin lk(&g_fileLock);
        r->state = kKeepFree;
      }
    }
  }
  oRtlExitUserThread(status);
}

// Game thread, slots changed: which threads the slots know.
struct Slot;
void RebuildKnown();
// Kept threads no valid slot knows any more: parked ones end for real, frozen ones are ended (game thread).
int ReleaseKept() {
  int n = 0;
  for (KeepRec& k : g_keep) {
    long st;
    {
      Spin lk(&g_fileLock);
      st = k.state;
      if ((st != kKeepParked && st != kKeepFrozen) || KnownMaskNoLock(k.tid, k.created)) continue;
      if (st == kKeepFrozen) {
        k.state = kKeepFree;
        --g_keepLive;
      } else {
        k.state = kKeepReleasing;  // the thread frees the record when it is out
      }
      RebuildDeadNoLock();
    }
    if (st == kKeepParked) {
      k.park.cmd = 2;
      SetEvent(k.park.ev);
    } else {
      oTerminateThread(k.h, k.exitCode);
      CloseHandle(k.h);
      k.h = nullptr;
    }
    ++n;
    ++g_released;
  }
  return n;
}

// #28: critical sections. The CRITICAL_SECTION is in game memory (the snapshot), its DebugInfo in the process heap,
// linked into ntdll's list of all critical sections (not in the snapshot). A map opened again deletes the old map's
// sections (DebugInfo unlinked and freed); a load brings back sections pointing at freed DebugInfo, and their next
// delete unlinks garbage -> fail fast c0000409/3 in RtlDeleteCriticalSection (#28: Load checkpoint series, 9th
// round). While a slot is valid the game's DeleteCriticalSection is held back: the section's DebugInfo stays
// allocated and linked (~32 bytes each; a delete later, with no slot, frees it as usual).
using DelCs_t = void(WINAPI*)(LPCRITICAL_SECTION);
DelCs_t oDeleteCs = nullptr;
std::atomic<long> g_csHeld{0};
bool AnySlotValid();
void WINAPI hkDeleteCriticalSection(LPCRITICAL_SECTION cs) {
  if (g_armed.load(std::memory_order_relaxed) && AnySlotValid()) {
    ++g_csHeld;
    return;
  }
  oDeleteCs(cs);
}

// Patch the waits / closes / TerminateThread of the exe and each dll of the game's folder (once per module).
std::set<uintptr_t> g_patchedMods;
int PatchModuleImports(uintptr_t base) {
  if (!g_patchedMods.insert(base).second) return 0;
  struct P {
    const char* f;
    void* det;
  } ps[] = {{"CloseHandle", (void*)&hkCloseHandle},
            {"WaitForSingleObject", (void*)&hkWFSO},
            {"WaitForSingleObjectEx", (void*)&hkWFSOEx},
            {"WaitForMultipleObjects", (void*)&hkWFMO},
            {"WaitForMultipleObjectsEx", (void*)&hkWFMOEx},
            {"GetExitCodeThread", (void*)&hkGECT},
            {"TerminateThread", (void*)&hkTerminateThread},
            {"DeleteCriticalSection", (void*)&hkDeleteCriticalSection}};
  int n = 0;
  for (P& p : ps)
    if (PatchImport(base, "KERNEL32.dll", p.f, p.det)) ++n;
  return n;
}

// ---------------------------------------------------------------------------------------------------------------
// Bink videos (#26, user's crash): binkw32 keeps its own state outside the snapshot (its data, heap, threads, files,
// sound). A load while a video plays (M02: the move into the "dream" streams the next part behind a video) brought
// back game memory that no longer knew the open video, while Bink went on using its buffers -> heap corruption 2 s
// later. The exe's _BinkOpen@8/_BinkClose@4 imports count the open videos; save/load wait until there are none.
using BinkOpen_t = void*(WINAPI*)(const char*, unsigned);
using BinkClose_t = void(WINAPI*)(void*);
BinkOpen_t oBinkOpen = nullptr;
BinkClose_t oBinkClose = nullptr;
// The main menu's background video stays open for the whole run: open videos are kept with the "map epoch" they were
// opened in (bumped by the game thread on every new world). Save: no video of the current map open. Load: the open
// videos must be exactly those of the save.
std::atomic<int> g_binkOpen{0};
std::atomic<long> g_binkOpened{0};
std::atomic<unsigned> g_mapEpoch{1};
// #29: two kinds. A full-screen movie (a .bik file: cutscene videos, the menu background) - save waits until it ends,
// a load stops one that started after the save (Engine.StopMovie). An in-world video (a movie texture: opened from
// memory, the name is the file's "BIK" header - the screens of the M14 boss arena play the whole fight) is kept like
// the game's files: while a slot knows it (open at its save), the game's BinkClose only marks it closed ("zombie");
// a load makes the slot's ones live again and closes those opened after the save that no slot knows.
struct Video {
  void* h;
  unsigned epoch;
  bool inWorld;
  bool zombie;
  uint8_t mask;  // slots that saved it open (in-world ones)
};
Video g_videos[32];
volatile LONG g_videoLock = 0;

void* WINAPI hkBinkOpen(const char* name, unsigned flags) {
  void* h = oBinkOpen(name, flags);
  bool inWorld = name && name[0] == 'B' && name[1] == 'I' && name[2] == 'K';
  if (h) {
    Spin lk(&g_videoLock);
    for (Video& v : g_videos)
      if (!v.h) {
        v = {h, g_mapEpoch.load(), inWorld, false, 0};
        break;
      }
    ++g_binkOpen;
    ++g_binkOpened;
  }
  LOG("savestate: video opened %s (%p, flags %X, %s, %d open)", inWorld ? "(in-world, from memory)" : name ? name : "?",
      h, flags, inWorld ? "kept for slots" : "full-screen", g_binkOpen.load());
  return h;
}

void WINAPI hkBinkClose(void* h) {
  bool held = false;
  if (h) {
    Spin lk(&g_videoLock);
    for (Video& v : g_videos)
      if (v.h == h) {
        if (v.inWorld && v.mask && g_armed) {  // a slot knows it: closed for the game, kept open
          held = !v.zombie;
          v.zombie = true;
        } else {
          v.h = nullptr;
        }
        --g_binkOpen;
      }
  }
  if (!held) oBinkClose(h);
  LOG("savestate: video closed %p%s (%d open)", h, held ? " - kept open for a slot" : "", g_binkOpen.load());
}

// Open videos (not zombies): all, or the full-screen ones (fileOnly), of this map only (thisMapOnly).
std::vector<void*> OpenVideos(bool thisMapOnly, bool fileOnly = false) {
  std::vector<void*> out;
  Spin lk(&g_videoLock);
  for (const Video& v : g_videos)
    if (v.h && !v.zombie && (!thisMapOnly || v.epoch == g_mapEpoch.load()) && !(fileOnly && v.inWorld))
      out.push_back(v.h);
  std::sort(out.begin(), out.end());
  return out;
}

void VideosSave(int s) {  // in-world videos open now belong to slot s
  Spin lk(&g_videoLock);
  for (Video& v : g_videos)
    if (v.h && v.inWorld) {
      if (!v.zombie) v.mask |= 1 << s;
      else v.mask &= ~(1 << s);
    }
}

// After a load of slot s (game thread, not paused): its videos are the game's again; in-world videos it does not
// know (opened after the save) are closed unless another slot knows them. Returns closed / revived counts.
void VideosLoad(int s, int* revived, int* closed) {
  *revived = *closed = 0;
  std::vector<void*> close;
  {
    Spin lk(&g_videoLock);
    for (Video& v : g_videos) {
      if (!v.h || !v.inWorld) continue;
      if (v.mask & (1 << s)) {
        if (v.zombie) v.zombie = false, ++g_binkOpen, ++*revived;
      } else if (v.mask) {
        if (!v.zombie) v.zombie = true, --g_binkOpen;
      } else {
        close.push_back(v.h);
        if (!v.zombie) --g_binkOpen;
        v.h = nullptr;
      }
    }
  }
  for (void* h : close) oBinkClose(h), ++*closed;
}

void VideosDropSlot(int s) {  // game thread
  std::vector<void*> close;
  {
    Spin lk(&g_videoLock);
    for (Video& v : g_videos) {
      if (!v.h || !v.inWorld) continue;
      v.mask &= ~(1 << s);
      if (!v.mask && v.zombie) close.push_back(v.h), v.h = nullptr;
    }
  }
  for (void* h : close) oBinkClose(h);
}

// ---------------------------------------------------------------------------------------------------------------
// D3D9 resources. Game memory holds pointers to D3D objects that live outside it (d3d9 runtime / driver). A load
// brings back pointers to objects the game may have released after the save. While save states are on (Dev), every
// resource the device creates gets one extra reference of ours ("pin"); the render thread drops the pin once the
// game holds no reference and no slot needs it. A slot remembers the game's reference count of each pinned object;
// a load raises a lower count back (never lowers one: an extra reference only leaks, a missing one would crash).
struct Pin {
  IUnknown* obj;
  int slotRefs[kSlots];
  uint8_t mask;
  bool keep;  // #29: survives a device Reset (managed / system-memory pool, shaders, declarations): stays pinned
};
std::vector<Pin> g_pins;
volatile LONG g_pinLock = 0;
size_t g_pinScan = 0;
std::atomic<long> g_pinCreated{0}, g_pinDropped{0}, g_pinRefsAdded{0};

int GameRefs(IUnknown* o) {  // references other than our pin
  ULONG r = o->AddRef();
  o->Release();
  return (int)r - 2;
}

uintptr_t g_d3dxBase = 0, g_d3dxEnd = 0;  // d3dx9_43 (the exe's texture/shader helper): counts as the game

// Only resources the game makes (exe, or d3dx called by it) are pinned: the tool's own (ImGui, layers) and the
// Steam overlay's are not in game memory and keep their own lifetime.
bool GameCreator(uintptr_t ret) {
  if (ret >= g_exeBase && ret < g_exeEnd) return true;
  if (!g_d3dxBase)
    if (HMODULE m = GetModuleHandleW(L"d3dx9_43.dll")) {
      MODULEINFO mi{};
      GetModuleInformation(GetCurrentProcess(), m, &mi, sizeof mi);
      g_d3dxEnd = (uintptr_t)mi.lpBaseOfDll + mi.SizeOfImage;
      g_d3dxBase = (uintptr_t)mi.lpBaseOfDll;
    }
  return ret >= g_d3dxBase && ret < g_d3dxEnd;
}

bool g_early = false;  // tracking since the game's device was created
void Track(IUnknown* o, uintptr_t ret, bool keep) {
  // #28: on from the start = always tracked, also while save states are briefly off: a resource made while off and
  // released by a map reload (Restart) would be a dangling pointer in a slot saved later (crash in d3d9, 17:49)
  if (!o || !(g_armed || g_early) || !GameCreator(ret)) return;
  o->AddRef();
  Spin lk(&g_pinLock);
  g_pins.push_back({o, {}, 0, keep});
  ++g_pinCreated;
}

// keep: the resource outlives a device Reset (#29) - not a D3DPOOL_DEFAULT one, render target, depth surface,
// state block or query (those must all be gone before Reset; the game makes them again).
#define SS_DEV_HOOK(idx, name, params, args, out, keep)                                    \
  using name##_t = HRESULT(WINAPI*) params;                                                 \
  name##_t o##name = nullptr;                                                               \
  HRESULT WINAPI hk##name params {                                                          \
    HRESULT hr = o##name args;                                                              \
    if (SUCCEEDED(hr) && (out)) Track(reinterpret_cast<IUnknown*>(*(out)), (uintptr_t)_ReturnAddress(), keep); \
    return hr;                                                                              \
  }

SS_DEV_HOOK(23, CreateTexture, (IDirect3DDevice9* d, UINT w, UINT h, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p,
                                IDirect3DTexture9** pp, HANDLE* sh), (d, w, h, l, u, f, p, pp, sh), pp, p != D3DPOOL_DEFAULT)
SS_DEV_HOOK(24, CreateVolumeTexture, (IDirect3DDevice9* d, UINT w, UINT h, UINT z, UINT l, DWORD u, D3DFORMAT f,
                                      D3DPOOL p, IDirect3DVolumeTexture9** pp, HANDLE* sh),
            (d, w, h, z, l, u, f, p, pp, sh), pp, p != D3DPOOL_DEFAULT)
SS_DEV_HOOK(25, CreateCubeTexture, (IDirect3DDevice9* d, UINT e, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p,
                                    IDirect3DCubeTexture9** pp, HANDLE* sh), (d, e, l, u, f, p, pp, sh), pp, p != D3DPOOL_DEFAULT)
SS_DEV_HOOK(26, CreateVertexBuffer, (IDirect3DDevice9* d, UINT n, DWORD u, DWORD fvf, D3DPOOL p,
                                     IDirect3DVertexBuffer9** pp, HANDLE* sh), (d, n, u, fvf, p, pp, sh), pp, p != D3DPOOL_DEFAULT)
SS_DEV_HOOK(27, CreateIndexBuffer, (IDirect3DDevice9* d, UINT n, DWORD u, D3DFORMAT f, D3DPOOL p,
                                    IDirect3DIndexBuffer9** pp, HANDLE* sh), (d, n, u, f, p, pp, sh), pp, p != D3DPOOL_DEFAULT)
SS_DEV_HOOK(28, CreateRenderTarget, (IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE m,
                                     DWORD q, BOOL lk, IDirect3DSurface9** pp, HANDLE* sh),
            (d, w, h, f, m, q, lk, pp, sh), pp, false)
SS_DEV_HOOK(29, CreateDepthStencilSurface, (IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT f,
                                            D3DMULTISAMPLE_TYPE m, DWORD q, BOOL dc, IDirect3DSurface9** pp,
                                            HANDLE* sh), (d, w, h, f, m, q, dc, pp, sh), pp, false)
SS_DEV_HOOK(36, CreateOffscreenPlainSurface, (IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT f, D3DPOOL p,
                                              IDirect3DSurface9** pp, HANDLE* sh), (d, w, h, f, p, pp, sh), pp, p != D3DPOOL_DEFAULT)
SS_DEV_HOOK(59, CreateStateBlock, (IDirect3DDevice9* d, D3DSTATEBLOCKTYPE t, IDirect3DStateBlock9** pp), (d, t, pp), pp, false)
SS_DEV_HOOK(86, CreateVertexDeclaration, (IDirect3DDevice9* d, const D3DVERTEXELEMENT9* e,
                                          IDirect3DVertexDeclaration9** pp), (d, e, pp), pp, true)
SS_DEV_HOOK(91, CreateVertexShader, (IDirect3DDevice9* d, const DWORD* c, IDirect3DVertexShader9** pp), (d, c, pp), pp, true)
SS_DEV_HOOK(106, CreatePixelShader, (IDirect3DDevice9* d, const DWORD* c, IDirect3DPixelShader9** pp), (d, c, pp), pp, true)
SS_DEV_HOOK(118, CreateQuery, (IDirect3DDevice9* d, D3DQUERYTYPE t, IDirect3DQuery9** pp), (d, t, pp), pp, false)

bool g_devHooked = false;
bool InstallDeviceHooks(void** vt = nullptr) {
  if (g_devHooked) return true;
  struct H {
    int idx;
    void* det;
    void** orig;
    const char* name;
  } hs[] = {
      {23, (void*)&hkCreateTexture, (void**)&oCreateTexture, "D3D9.CreateTexture"},
      {24, (void*)&hkCreateVolumeTexture, (void**)&oCreateVolumeTexture, "D3D9.CreateVolumeTexture"},
      {25, (void*)&hkCreateCubeTexture, (void**)&oCreateCubeTexture, "D3D9.CreateCubeTexture"},
      {26, (void*)&hkCreateVertexBuffer, (void**)&oCreateVertexBuffer, "D3D9.CreateVertexBuffer"},
      {27, (void*)&hkCreateIndexBuffer, (void**)&oCreateIndexBuffer, "D3D9.CreateIndexBuffer"},
      {28, (void*)&hkCreateRenderTarget, (void**)&oCreateRenderTarget, "D3D9.CreateRenderTarget"},
      {29, (void*)&hkCreateDepthStencilSurface, (void**)&oCreateDepthStencilSurface, "D3D9.CreateDepthStencilSurface"},
      {36, (void*)&hkCreateOffscreenPlainSurface, (void**)&oCreateOffscreenPlainSurface, "D3D9.CreateOffscreenPlainSurface"},
      {59, (void*)&hkCreateStateBlock, (void**)&oCreateStateBlock, "D3D9.CreateStateBlock"},
      {86, (void*)&hkCreateVertexDeclaration, (void**)&oCreateVertexDeclaration, "D3D9.CreateVertexDeclaration"},
      {91, (void*)&hkCreateVertexShader, (void**)&oCreateVertexShader, "D3D9.CreateVertexShader"},
      {106, (void*)&hkCreatePixelShader, (void**)&oCreatePixelShader, "D3D9.CreatePixelShader"},
      {118, (void*)&hkCreateQuery, (void**)&oCreateQuery, "D3D9.CreateQuery"},
  };
  for (H& h : hs) {
    void* target = vt ? vt[h.idx] : d3d9hook::DeviceVtableEntry(h.idx);
    if (!target || !hooks::Create(target, h.det, h.orig, h.name) || !hooks::Enable(target, h.name)) return false;
  }
  g_devHooked = true;
  return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Pausing the game. On the game thread at the start of FEngineLoop::Tick (between two frames: its stack is only
// WinMain -> main loop -> us), every other thread of the game (started in the exe, a dll of the game's folder or
// msvcr100) is suspended; all of them must sit in a wait system call and, if they came from game code, not inside
// a d3d9/driver call (the rendering thread in Present = not done with the frame yet). Otherwise resume and retry.
enum ModKind : uint8_t { kSys, kGame, kOther, kSelf, kGfx };  // kGfx: d3d9 / dxgi / GPU driver
struct ModRange {
  uintptr_t base, end;
  ModKind kind;
  std::string name;
  bool phys;  // PhysX / APEX / msvcr90 (PhysX's CRT): physics state outside UE3's allocator
  bool crt;   // #27: msvcr100 / msvcp100 / a dll of the game's folder linked to them: their .data points into the
              // msvcr100 heap of the snapshot (msvcr100's atexit table, msvcp100's facet list) - in the snapshot too
};

// #27: does a loaded image import msvcr100.dll / msvcp100.dll?
bool ImportsCrt100(uintptr_t base) {
  __try {
    auto dos = (const IMAGE_DOS_HEADER*)base;
    auto nt = (const IMAGE_NT_HEADERS32*)(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd.VirtualAddress) return false;
    for (auto d = (const IMAGE_IMPORT_DESCRIPTOR*)(base + dd.VirtualAddress); d->Name; ++d) {
      const char* n = (const char*)(base + d->Name);
      if (!_stricmp(n, "msvcr100.dll") || !_stricmp(n, "msvcp100.dll")) return true;
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return false;
}
std::vector<ModRange> g_mods;
std::vector<std::pair<uintptr_t, uintptr_t>> g_waitStubs;  // [start, end) of allowed wait syscall stubs

const ModRange* ModOf(uintptr_t a) {
  auto it = std::upper_bound(g_mods.begin(), g_mods.end(), a, [](uintptr_t v, const ModRange& m) { return v < m.base; });
  if (it == g_mods.begin()) return nullptr;
  --it;
  return a < it->end ? &*it : nullptr;
}

void RefreshModules() {
  std::vector<Module> mods = Modules();
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     (LPCWSTR)&RefreshModules, &self);
  std::wstring exeDir = util::ExeDir();
  std::vector<ModRange> out;
  for (const Module& m : mods) {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW((HMODULE)m.base, path, MAX_PATH);
    std::wstring p = path;
    std::wstring dir = p.substr(0, p.find_last_of(L'\\') + 1);
    const char* n = m.name.c_str();
    ModKind k = kOther;
    if ((HMODULE)m.base == self) k = kSelf;
    else if (_wcsicmp(dir.c_str(), exeDir.c_str()) == 0) k = kGame;
    else if (!_stricmp(n, "ntdll.dll") || !_stricmp(n, "KERNELBASE.dll") || !_stricmp(n, "KERNEL32.DLL") ||
             !_stricmp(n, "win32u.dll") || !_stricmp(n, "user32.dll") || !_stricmp(n, "msvcr100.dll") ||
             !_stricmp(n, "msvcp100.dll") || !_stricmp(n, "msvcr90.dll") || !_stricmp(n, "ucrtbase.dll") ||
             !_stricmp(n, "gameoverlayrenderer.dll"))
      k = kSys;
    else if (!_strnicmp(n, "d3d", 3) || !_strnicmp(n, "dxgi", 4) || !_strnicmp(n, "nv", 2) ||
             !_strnicmp(n, "ati", 3) || !_strnicmp(n, "amd", 3) || !_strnicmp(n, "igd", 3) || !_strnicmp(n, "igx", 3))
      k = kGfx;
    bool phys = !_strnicmp(n, "NTPhysX", 7) || !_strnicmp(n, "APEX", 4) || !_strnicmp(n, "PhysX", 5) ||
                !_stricmp(n, "msvcr90.dll");
    bool crt = !_stricmp(n, "msvcr100.dll") || !_stricmp(n, "msvcp100.dll") ||
               (k == kGame && (uintptr_t)m.base != (uintptr_t)GetModuleHandleW(nullptr) && ImportsCrt100(m.base));
    out.push_back({m.base, m.end, k, m.name, phys, crt});
  }
  g_mods = std::move(out);
  if (g_waitStubs.empty()) {
    const char* nt[] = {"NtWaitForSingleObject", "NtWaitForMultipleObjects", "NtDelayExecution",
                        "NtRemoveIoCompletion", "NtRemoveIoCompletionEx", "NtWaitForAlertByThreadId",
                        "NtWaitForKeyedEvent", "NtSignalAndWaitForSingleObject", "NtWaitForWorkViaWorkerFactory",
                        "NtWaitForMultipleObjects32", "NtReplyWaitReceivePort", "NtAlpcSendWaitReceivePort"};
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    for (const char* f : nt)
      if (auto a = (uintptr_t)GetProcAddress(ntdll, f)) g_waitStubs.push_back({a, a + 0x20});
    if (HMODULE w = GetModuleHandleW(L"win32u.dll")) {
      const char* wu[] = {"NtUserGetMessage", "NtUserMsgWaitForMultipleObjectsEx", "NtUserWaitMessage",
                          "NtUserRealWaitMessageEx"};
      for (const char* f : wu)
        if (auto a = (uintptr_t)GetProcAddress(w, f)) g_waitStubs.push_back({a, a + 0x20});
    }
  }
}

// #28: the exe and every dll of the game's folder: their waits / closes / TerminateThread (see "kept threads").
bool InLockWait(uintptr_t eip);
void PatchGameModules() {
  InLockWait(0);  // its stub addresses, outside any pause (GetProcAddress)
  int n = 0, mods = 0;
  for (const ModRange& m : g_mods) {
    if (m.kind == kGame)
      if (int k = PatchModuleImports(m.base)) n += k, ++mods;
    if (m.crt && m.kind != kGame && g_patchedMods.insert(m.base).second)  // msvcr100 / msvcp100: their heaps are in the
      if (PatchImport(m.base, "KERNEL32.dll", "DeleteCriticalSection", (void*)&hkDeleteCriticalSection))  // snapshot
        ++n, ++mods;
  }
  if (n) LOG("savestate: %d imports patched in %d game module(s) (waits, CloseHandle, TerminateThread)", n, mods);
}

// #28: lock waits (critical sections / SRW: a thread there may hold another lock) - never "idle"
bool InLockWait(uintptr_t eip) {
  static uintptr_t a = 0, b = 0;
  if (!a) {
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    a = (uintptr_t)GetProcAddress(nt, "NtWaitForAlertByThreadId");
    b = (uintptr_t)GetProcAddress(nt, "NtWaitForKeyedEvent");
  }
  return (a && eip >= a && eip < a + 0x20) || (b && eip >= b && eip < b + 0x20);
}

uintptr_t StubStart(uintptr_t eip) {  // #28: the wait stub eip is in
  for (auto& [a, b] : g_waitStubs)
    if (eip >= a && eip < b) return a;
  return 0;
}

bool InWaitStub(uintptr_t eip) {
  for (auto& [a, b] : g_waitStubs)
    if (eip >= a && eip < b) return true;
  return false;
}

struct PThread {
  DWORD tid;
  HANDLE h;
  uintptr_t start, stackBase, teb;
  bool mustIdle;  // started by the game: must wait idle; others (driver, COM, CLR, thread pool) are only frozen
  bool suspended;
  uintptr_t eip;
  int why;  // 0 ok, 1 running, 2 inside a d3d9/driver call
  uintptr_t esp, ebx, esi, edi, ebp;  // where it waits (TryPause)
  uintptr_t gameRet;                   // first return address into game code on its stack (the wait's caller)
  uint64_t created;                    // #28: creation time (a thread id can be reused)
};
std::vector<PThread> g_pt;
// #27: every thread of the process but the game thread (the tool's too), for their FLS slots (see ForFls).
struct PtdThread {
  DWORD tid;
  uintptr_t teb;
  uint64_t created;
};
std::vector<PtdThread> g_ptdThreads;
uintptr_t g_tickFrame = 0;  // game thread: first byte above hkTick's return address
uintptr_t g_stackBuf[1024];

void ListGameThreads() {
  for (PThread& t : g_pt) CloseHandle(t.h);
  g_pt.clear();
  g_pt.reserve(512);
  DWORD self = GetCurrentThreadId();
  g_ptdThreads.clear();
  g_ptdThreads.reserve(512);
  for (DWORD tid : ThreadIds()) {
    if (tid == self) continue;
    ThreadInfo ti = Inspect(tid, false);
    if (ti.teb) g_ptdThreads.push_back({tid, ti.teb, ti.created});
    const ModRange* m = ModOf(ti.start);
    if (m && m->kind == kSelf) continue;  // the tool's own threads keep running (they never touch game memory)
    bool game = m && (m->kind == kGame || !_stricmp(m->name.c_str(), "msvcr100.dll"));
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | THREAD_TERMINATE |
                              SYNCHRONIZE,
                          FALSE, tid);
    if (h) g_pt.push_back({tid, h, ti.start, ti.stackBase, ti.teb, game, false, 0, 0, 0, 0, 0, 0, 0, 0, ti.created});
  }
}

bool OwnHandle(HANDLE h) {
  for (const PThread& t : g_pt)
    if (t.h == h) return true;
  return false;
}

void ResumeAll() {
  for (PThread& t : g_pt)
    if (t.suspended) {
      ResumeThread(t.h);
      t.suspended = false;
    }
}

// Suspend + check once. No heap, no logging.
bool TryPause(int* bad) {
  for (PThread& t : g_pt) t.suspended = SuspendThread(t.h) != (DWORD)-1;
  for (PThread& t : g_pt)
    if (t.suspended && !t.mustIdle) {
      CONTEXT c{};
      c.ContextFlags = CONTEXT_CONTROL;
      GetThreadContext(t.h, &c);  // returns once the thread is really stopped
    }
  for (size_t i = 0; i < g_pt.size(); ++i) {
    PThread& t = g_pt[i];
    if (!t.suspended || !t.mustIdle) continue;
    CONTEXT c{};
    c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (!GetThreadContext(t.h, &c)) continue;  // gone
    t.eip = c.Eip;
    t.esp = c.Esp;
    t.ebx = c.Ebx;
    t.esi = c.Esi;
    t.edi = c.Edi;
    t.ebp = c.Ebp;
    t.why = 0;
    t.gameRet = 0;
    if (!InWaitStub(c.Eip)) {
      t.why = 1;
      *bad = (int)i;
      return false;
    }
    size_t n = t.stackBase > c.Esp ? std::min<size_t>((t.stackBase - c.Esp) / 4, std::size(g_stackBuf)) : 0;
    if (!n || !util::SafeRead(c.Esp, g_stackBuf, n * 4)) continue;
    for (size_t k = 0; k < n; ++k) {
      const ModRange* m = ModOf(g_stackBuf[k]);
      if (!m || m->kind == kSys) continue;
      uintptr_t tg;
      const char* kind;
      if (!CallBefore(g_stackBuf[k], &tg, &kind)) continue;
      if (m->kind == kOther) continue;  // e.g. a network thread waiting in mswsock
      if (m->kind == kGfx) {  // the rendering thread in Present / a driver call: the frame is not done yet
        // #27: ...unless the driver only hooks a wait the game called itself (nvgpucomp32 patches KERNELBASE's
        // WaitForSingleObjectEx: exe -> KERNELBASE -> nvgpucomp32 -> ntdll wait). Next real frame after the
        // driver's is KERNELBASE / KERNEL32 = a hooked wait; d3d9 / the exe = a real driver call.
        bool hookedWait = false;
        for (size_t j = k + 1; j < n; ++j) {
          const ModRange* m2 = ModOf(g_stackBuf[j]);
          uintptr_t tg2;
          const char* kind2;
          if (!m2 || !CallBefore(g_stackBuf[j], &tg2, &kind2)) continue;
          if (m2->kind == kGfx) break;
          hookedWait = m2->kind == kSys && (!_stricmp(m2->name.c_str(), "KERNELBASE.dll") ||
                                            !_stricmp(m2->name.c_str(), "KERNEL32.DLL"));
          if (hookedWait) k = j;
          break;
        }
        if (hookedWait) continue;
        t.why = 2;
        t.eip = g_stackBuf[k];
        *bad = (int)i;
        return false;
      }
      t.gameRet = g_stackBuf[k];
      break;  // first frame of game code: an idle wait of the game
    }
  }
  return true;
}

std::string Fmt(const char* f, ...);

// #26: where each game thread usually waits ("rest"). A tool thread stops one game thread at a time for a moment
// (~4 per second), reads where it waits (first return address into the exe, stack depth) and counts it; the most
// seen place is its rest. A save waits until every game thread is at its rest: a thread caught inside a task
// (AsyncIOSystem inside a read while a zone streams) would make the slot unloadable (its stack goes back only when
// it waits at the same place again). Fixed tables, no heap in the sampler.
struct RestRec {
  DWORD tid;
  struct {
    uintptr_t ret;
    uint32_t depth, n;
  } pos[4];
  uint32_t samples;
};
RestRec g_rest[256];
volatile LONG g_restLock = 0;
std::atomic<int> g_restN{0};
std::atomic<long> g_restSamples{0};

RestRec* RestOfNoLock(DWORD tid, bool add) {
  int n = g_restN.load();
  for (int i = 0; i < n; ++i)
    if (g_rest[i].tid == tid) return &g_rest[i];
  if (!add || n >= (int)std::size(g_rest)) return nullptr;
  g_rest[n] = {};
  g_rest[n].tid = tid;
  g_restN = n + 1;
  return &g_rest[n];
}

// the rest place of a thread: ret/depth seen most (and at least 6 samples, more than half of them)
bool RestOf(DWORD tid, uintptr_t* ret, uint32_t* depth) {
  Spin lk(&g_restLock);
  RestRec* r = RestOfNoLock(tid, false);
  if (!r || r->samples < 6) return false;
  int best = 0;
  for (int i = 1; i < 4; ++i)
    if (r->pos[i].n > r->pos[best].n) best = i;
  if (r->pos[best].n * 2 < r->samples) return false;
  *ret = r->pos[best].ret;
  *depth = r->pos[best].depth;
  return true;
}

// #29: a thread waits at one of its usual places: the most seen one, or another one seen often lately (the counts
// fade, see SampleOne). The rendering thread of the M14 boss arena waits 28 bytes deeper than in the rest of the game
// (movie textures): the only "most seen" place kept every save there "busy" for 5 s.
bool RestOk(DWORD tid, uint32_t depth) {
  Spin lk(&g_restLock);
  RestRec* r = RestOfNoLock(tid, false);
  if (!r || r->samples < 6) return true;  // not known well enough: as before (no rest = no check)
  uint32_t top = 0;
  for (int i = 0; i < 4; ++i) top = std::max(top, r->pos[i].n);
  for (int i = 0; i < 4; ++i)
    if (r->pos[i].depth == depth && r->pos[i].n && (r->pos[i].n == top || (r->pos[i].n >= 4 && r->pos[i].n * 4 >= top)))
      return true;
  return false;
}

bool InWaitStub(uintptr_t eip);
bool CallBefore(uintptr_t ret, uintptr_t* target, const char** kind);
uintptr_t g_sampleStack[1024], g_checkStack[1024];

// The first return address into the exe on a stopped thread's stack (where in the game it waits). No heap.
uintptr_t ExeWaitCaller(uintptr_t esp, uintptr_t stackBase, uintptr_t* buf) {
  if (stackBase <= esp) return 0;
  size_t n = std::min<size_t>((stackBase - esp) / 4, 1024);
  if (!util::SafeRead(esp, buf, n * 4)) return 0;
  for (size_t k = 0; k < n; ++k) {
    uintptr_t v = buf[k], tg;
    const char* kind;
    if (v >= g_exeBase && v < g_exeEnd && CallBefore(v, &tg, &kind)) return v;
  }
  return 0;
}

void SampleOne(DWORD tid, HANDLE h, uintptr_t stackBase) {
  if (SuspendThread(h) == (DWORD)-1) return;
  CONTEXT c{};
  c.ContextFlags = CONTEXT_CONTROL;
  uintptr_t ret = 0;
  uint32_t depth = 0;
  bool ok = GetThreadContext(h, &c) && InWaitStub(c.Eip) && stackBase > c.Esp;
  if (ok) {
    ret = ExeWaitCaller(c.Esp, stackBase, g_sampleStack);
    depth = (uint32_t)(stackBase - c.Esp);
  }
  ResumeThread(h);
  if (!ok) return;
  Spin lk(&g_restLock);
  RestRec* r = RestOfNoLock(tid, true);
  if (!r) return;
  ++r->samples;
  ++g_restSamples;
  int slot = -1, low = 0;
  for (int i = 0; i < 4; ++i) {
    if (r->pos[i].n && r->pos[i].depth == depth) slot = i;  // the same depth = the same wait (a raw stack scan's
                                                              // first exe address can be a stale value)
    if (r->pos[i].n < r->pos[low].n) low = i;
  }
  if (slot < 0) {  // a new place takes the least seen one
    slot = low;
    r->pos[slot] = {ret, depth, 0};
  }
  ++r->pos[slot].n;
  if (r->samples >= 64) {  // #29: older samples fade (a new level / arena brings its own usual waits)
    r->samples /= 2;
    for (int i = 0; i < 4; ++i) r->pos[i].n /= 2;
  }
}

std::atomic<bool> g_samplerOn{false};
DWORD WINAPI SamplerThread(LPVOID) {
  struct T {
    DWORD tid;
    HANDLE h;
    uintptr_t base;
  };
  std::vector<T> ts;
  DWORD lastList = 0;
  size_t next = 0;
  uintptr_t exeLo = g_exeBase, exeHi = g_exeEnd;
  HMODULE crt = GetModuleHandleW(L"msvcr100.dll");
  MODULEINFO ci{};
  if (crt) GetModuleInformation(GetCurrentProcess(), crt, &ci, sizeof ci);
  for (;;) {
    Sleep(25);  // #29 (was 60): ~35 game threads, each seen about every second
    if (!g_samplerOn) continue;
    if (GetTickCount() - lastList > 10000 || ts.empty()) {  // the game's threads (started in the exe / msvcr100)
      lastList = GetTickCount();
      for (T& t : ts) CloseHandle(t.h);
      ts.clear();
      for (DWORD tid : ThreadIds()) {
        if (tid == GetCurrentThreadId()) continue;
        ThreadInfo ti = Inspect(tid, false);
        bool game = (ti.start >= exeLo && ti.start < exeHi) ||
                    (crt && ti.start >= (uintptr_t)ci.lpBaseOfDll && ti.start < (uintptr_t)ci.lpBaseOfDll + ci.SizeOfImage);
        if (!game || !ti.stackBase) continue;
        if (HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tid)) ts.push_back({tid, h, ti.stackBase});
      }
      next = 0;
      {  // #28: forget threads that ended (map changes make new ones; the table is fixed)
        Spin lk(&g_restLock);
        int n = g_restN.load(), o = 0;
        for (int i = 0; i < n; ++i) {
          bool alive = false;
          for (const T& t : ts) alive |= t.tid == g_rest[i].tid;
          if (alive) g_rest[o++] = g_rest[i];
        }
        g_restN = o;
      }
    }
    if (ts.empty()) continue;
    if (next >= ts.size()) next = 0;
    T& t = ts[next++];
    SampleOne(t.tid, t.h, t.base);
  }
}

void RefreshModules();
void StartSampler() {
  static bool started = false;
  if (g_waitStubs.empty()) RefreshModules();  // the wait stubs (ntdll/win32u), once
  g_samplerOn = true;
  if (started) return;
  started = true;
  if (HANDLE h = CreateThread(nullptr, 0, SamplerThread, nullptr, 0, nullptr)) CloseHandle(h);
}

bool TryLock(volatile LONG* l) {
  for (int i = 0; i < 2000; ++i) {
    if (InterlockedCompareExchange(l, 1, 0) == 0) return true;
    YieldProcessor();
  }
  return false;
}

// Pause with our two tables locked (a frozen thread could hold them otherwise). False + *bad = the busy thread.
bool Pause(int* tries, int* bad) {
  for (*tries = 0; *tries < 400; ++*tries) {
    if (TryPause(bad)) {
      if (TryLock(&g_vaLock)) {
        if (TryLock(&g_pinLock)) {
          if (TryLock(&g_fileLock)) {
            if (TryLock(&g_enumLock)) return true;  // #28
            InterlockedExchange(&g_fileLock, 0);
          }
          InterlockedExchange(&g_pinLock, 0);
        }
        InterlockedExchange(&g_vaLock, 0);
      }
      *bad = -1;
    }
    ResumeAll();
    Sleep(*tries < 50 ? 0 : 1);
  }
  return false;
}

void Unpause() {
  InterlockedExchange(&g_enumLock, 0);
  InterlockedExchange(&g_fileLock, 0);
  InterlockedExchange(&g_pinLock, 0);
  InterlockedExchange(&g_vaLock, 0);
  ResumeAll();
}

std::string NotStopped(const char* what, int bad) {
  if (bad < 0 || bad >= (int)g_pt.size()) return Fmt("%s: the game did not stop (tool tables busy)", what);
  PThread& b = g_pt[bad];
  return Fmt("%s: the game did not stop (thread %lu %s at %s%s)", what, b.tid, NameOf(b.tid).c_str(),
             Where(Modules(), b.eip).c_str(), b.why == 2 ? ", inside a driver call" : "");
}

// ---------------------------------------------------------------------------------------------------------------
// Slots. Game memory = writable pages of: the exe image (.data/.bss), VirtualAlloc regions the exe made (UE3's
// FMallocWindows pools and big blocks), optionally msvcr100's heap and the images of the game's own dlls. Copied into
// a pagefile-backed section (outside the game's 32-bit address space; mapped 64 MB at a time).
enum : unsigned { kIncExeData = 1, kIncExeVA = 2, kIncCrtHeap = 4, kIncGameDlls = 8, kIncCrtData = 16, kIncFls = 32 };
std::atomic<unsigned> g_inc{kIncExeData | kIncExeVA | kIncCrtHeap | kIncGameDlls | kIncCrtData | kIncFls};
std::atomic<bool> g_freeNew{true};

struct RunRec {
  uintptr_t addr;
  uint32_t size;
  uint8_t kind;  // 0 image, 1 exe VA, 2 crt heap
  uint64_t off;
};
struct AllocRec {
  uintptr_t base, extent, caller;
  uint8_t kind;  // 1 exe VA, 2 crt heap
};
struct Slot {
  bool valid = false;
  HANDLE sec = nullptr;
  uint64_t secSize = 0, used = 0;
  std::vector<RunRec> runs;
  std::vector<AllocRec> allocs;  // exe VirtualAlloc allocations at the save, by base
  std::string map;
  std::vector<uintptr_t> levels;  // loaded streaming levels (LoadedLevel, | 1 if visible)
  std::string levelNames;         // the same by name (log)
  uintptr_t world = 0;            // the UWorld object at the save (another one = the map was opened again)
  int files = 0;                  // game file handles open at the save
  int handles = 0;                // #28: kernel objects open at the save
  struct ThreadHandle {
    HANDLE h;
    DWORD tid;
    uint64_t created;
  };
  std::vector<ThreadHandle> threadHandles;  // #28: handle values naming its game threads (waits on them are sliced)
  int onexitN = -1;               // #27: entries of msvcr100's atexit table at the save (after a load: the same)
  std::vector<ThreadKey> threads;  // the game's threads at the save (sorted by tid; #28: + creation time, start)
  DWORD renderTid = 0;            // #27: the rendering thread at the save
  long long qpcVirtual = 0;
  float pos[3] = {0, 0, 0};
  int hp = 0;
  float hpMax = 0, dt = 0, dtMax = 0;  // #29: shown in the Save states tab
  std::string checkpoint;              // #29: the game's checkpoint at the save ("" = mission start)
  // #29: the slot's copy of the game's memory: raw (sec, until the packer is done) or packed (pages in the store).
  // gen changes with every change of the copy (under ssstore::Mu): a pack job of an older copy is dropped.
  ssstore::PageList pages;
  bool packed = false;
  uint32_t gen = 0;
  double pauseMs = 0, copyMs = 0, totalMs = 0;
  int pauseTries = 0, loads = 0;
  unsigned long long bytes[3] = {0, 0, 0};
  int pins = 0;
  SYSTEMTIME when{};
  HANDLE evH[4] = {};  // kernel events the restored memory waits on (FFrameEndSync) and their state at the save
  LONG evS[4] = {};
  int nEv = 0;
  std::vector<uintptr_t> heaps;  // heap handles taken (g_heapSet at the save)
  std::vector<void*> videos;     // Bink videos open at the save (the menu's background one)
  struct TlsVal {
    DWORD tid;
    uintptr_t teb;
    int index;
    uintptr_t value;
  };
  std::vector<TlsVal> tls;  // the game's TLS slots of the game threads
  struct FlsThread {  // #27: FLS slots of every thread at the save (see ForFls): the thread, its words [begin, end)
    DWORD tid;
    uint64_t created;
    uint32_t begin, end;
  };
  struct FlsVal {
    uint16_t pos;
    uintptr_t value;
  };
  std::vector<FlsThread> flsThreads;
  std::vector<FlsVal> fls;  // non-zero words only
  // Stacks of the game's threads (ESP..base) + their kept registers: Havok's worker threads keep their memory
  // router (per-thread free lists) on their own stack. The game thread: the frames above FEngineLoop::Tick.
  struct StackRec {
    DWORD tid;
    uintptr_t esp, eip, base, gameRet;
    uint32_t off, size;
    uintptr_t ebx, esi, edi, ebp;
    uintptr_t start, seh;    // #28: start address; SEH head (TEB+0) at the save
    uint32_t mxcsr;          // #28: MXCSR / x87 control word (a kept thread goes back with them)
    uint16_t fcw;
  };
  std::vector<StackRec> stacks;
  std::vector<uint8_t> stackData;
};
Slot g_slots[kSlots];

bool AnySlotValid() {
  for (const Slot& s : g_slots)
    if (s.valid) return true;
  return false;
}

void RebuildKnown() {
  Spin lk(&g_fileLock);
  g_knownN = 0;
  for (int s = 0; s < kSlots; ++s) {
    g_knownRender[s] = g_slots[s].valid ? g_slots[s].renderTid : 0;
    if (!g_slots[s].valid) continue;
    for (const ThreadKey& k : g_slots[s].threads) {
      int i = 0;
      for (; i < g_knownN; ++i)
        if (g_known[i].tid == k.tid && g_known[i].created == k.created) break;
      if (i == g_knownN) {
        if (g_knownN >= (int)std::size(g_known)) continue;
        g_known[g_knownN++] = {k.tid, k.created, 0};
      }
      g_known[i].mask |= 1 << s;
    }
  }
  int n = 0;
  for (int s = 0; s < kSlots; ++s)
    if (g_slots[s].valid)
      for (const Slot::ThreadHandle& th : g_slots[s].threadHandles)
        if (n < (int)std::size(g_watch) && std::find(g_watch, g_watch + n, th.h) == g_watch + n)
          g_watch[n] = th.h, g_watchTid[n] = th.tid, g_watchCreated[n] = th.created, ++n;
  g_watchN = n;
}

bool InAnySlotNoLock(uintptr_t base) {
  for (const Slot& sl : g_slots) {
    if (!sl.valid) continue;
    auto it = std::lower_bound(sl.allocs.begin(), sl.allocs.end(), base,
                               [](const AllocRec& x, uintptr_t v) { return x.base < v; });
    if (it != sl.allocs.end() && it->base == base) return true;
  }
  return false;
}

// #26: heap segments (msvcr100/physics heaps) allocated after a save are unknown to the heap once that save is
// loaded: "orphans", still allocated. Another slot may need them back; else they are freed. Sorted. Game thread /
// under g_vaLock. g_orphanTmp: the next list, reserved before a pause (no allocation while the game is frozen).
std::vector<uintptr_t> g_orphans, g_orphanTmp;
bool IsOrphan(uintptr_t b) { return std::binary_search(g_orphans.begin(), g_orphans.end(), b); }
int PruneOrphansNoLock() {
  int n = 0;
  for (size_t i = 0; i < g_orphans.size();) {
    if (!InAnySlotNoLock(g_orphans[i])) {
      oVirtualFree((void*)g_orphans[i], 0, MEM_RELEASE);
      g_orphans.erase(g_orphans.begin() + i);
      ++n;
    } else {
      ++i;
    }
  }
  return n;
}
bool g_retry = false;  // DoLoad: refused for now, ask again next frame (game threads not at their wait)

// #30: heap zombies no slot needs any more go back to the OS (released here past the hook). Caller holds g_vaLock.
int ReleaseHeapZombiesNoLock() {
  if (!g_hzZombieN || !HzTryLock(4000)) return 0;
  int n = 0;
  for (int i = 0; i < g_hzZombieN;) {
    uintptr_t b = g_hzZombie[i];
    if (InAnySlotNoLock(b)) {
      ++i;
      continue;
    }
    PVOID bb = (PVOID)b;
    SIZE_T sz = 0;
    oNtFreeVM((HANDLE)-1, &bb, &sz, MEM_RELEASE);
    g_hzZombie[i] = g_hzZombie[--g_hzZombieN];
    ++n;
    ++g_hzFreed;
  }
  HzUnlock();
  return n;
}

// Zombies no slot needs any more go back to the OS. Caller holds g_vaLock.
int ReleaseZombiesNoLock() {
  int n = ReleaseHeapZombiesNoLock();
  for (size_t i = 0; i < kVaSlots; ++i) {
    VaEntry& e = g_va[i];
    if (e.base <= 1 || !e.zombie || InAnySlotNoLock(e.base)) continue;
    uintptr_t b = e.base;
    if (oVirtualFree((void*)b, 0, MEM_RELEASE)) {
      VaRemoveNoLock(b);
      --g_zombies;
      ++n;
    }
  }
  return n;
}

std::vector<int> g_gameTls;  // TLS indices allocated by the exe or a dll of the game's folder (before a pause)
void GameTlsIndices() {
  g_gameTls.clear();
  for (int i = 0; i < kTlsMax; ++i)
    if (uintptr_t c = g_tlsOwner[i]) {
      const ModRange* m = ModOf(c);
      if (m && (m->kind == kGame || m->phys)) g_gameTls.push_back(i);
    }
}

uintptr_t* TlsCell(uintptr_t teb, int i) {  // 32-bit TEB: TlsSlots[64] at 0xE10, TlsExpansionSlots at 0xF94
  if (i < 64) return (uintptr_t*)(teb + 0xE10 + i * 4);
  uintptr_t exp = 0;
  if (!util::Read(teb + 0xF94, exp) || !exp) return nullptr;
  return (uintptr_t*)(exp + (i - 64) * 4);
}

// #27: FLS (fiber local storage) slots. CRTs keep their per-thread data (_ptd: errno, locale, strtok...) there:
// msvcr100, msvcr90 and the static CRTs of PhysX / APEX - created on the thread's first need, in the CRT's heap
// (in the snapshot), freed by the CRT's FLS callback when the thread ends. The slots themselves are outside the
// snapshot (ntdll's FLS data in the process heap). After a load, a thread that got its _ptd after the save points
// at a block the restored heap counts as free: when the thread ends, _freefls frees it again -> C0000374 (#27: a
// pool/driver thread, 25 s after a load) or the game gets that block meanwhile. A load puts back every FLS word
// that points into the snapshot as it was at the save; a thread born after the save gets NULL there (the CRT makes
// a new _ptd when that thread needs one and frees nothing when it ends).
// Layout (Windows 10 2004+, `ss fls`): TEB.FlsData (32-bit TEB +0xFB4) = {LIST_ENTRY; void* chunk[8]} (0x28 B),
// chunk k = 1 + (16 << k) words, word 0 not a slot. Checked once on the game thread (msvcr100's _ptd must be found
// there, block sizes must match), else FLS is left alone (logged).
std::vector<uintptr_t> g_crtSegs;
uint64_t g_gameCreated = 0;  // the game thread's creation time
int g_flsOk = 0;  // 0 not checked, 1 layout as expected, -1 not
std::string g_flsWhy = "FLS layout not checked yet (first save/load)";
template <class F>
void ForFls(uintptr_t teb, F f) {
  uintptr_t fd = 0;
  if (g_flsOk <= 0 || !teb || !util::Read(teb + 0xFB4, fd) || !fd) return;
  for (int k = 0; k < 8; ++k) {
    uintptr_t c = 0;
    if (!util::Read(fd + 8 + k * 4, c) || c < 0x10000) continue;
    for (uint32_t i = 1; i <= (16u << k); ++i) {
      uintptr_t v = 0;
      if (!util::Read(c + i * 4, v)) break;
      f((uint16_t)(k * 1024 + i), (uintptr_t*)(c + i * 4), v);
    }
  }
}
// Game thread, not paused (heap calls).
void CheckFlsLayout() {
  if (g_flsOk) return;
  g_flsOk = -1;
  HMODULE crt = GetModuleHandleW(L"msvcr100.dll");
  auto gp = crt ? (void*(__cdecl*)())GetProcAddress(crt, "_getptd") : nullptr;
  uintptr_t teb = (uintptr_t)NtCurrentTeb(), fd = 0;
  HANDLE ph = GetProcessHeap();
  if (!gp || !util::Read(teb + 0xFB4, fd) || !fd) {
    g_flsWhy = "FLS: no msvcr100!_getptd / TEB.FlsData - FLS slots NOT handled";
    return;
  }
  uintptr_t me = (uintptr_t)gp();
  FILETIME ct{}, et{}, kt{}, ut{};
  if (GetThreadTimes(GetCurrentThread(), &ct, &et, &kt, &ut))
    g_gameCreated = ((uint64_t)ct.dwHighDateTime << 32) | ct.dwLowDateTime;
  if (!HeapValidate(ph, 0, (void*)fd) || HeapSize(ph, 0, (void*)fd) != 0x28) {
    g_flsWhy = Fmt("FLS: FlsData %08X is not a 0x28 B block - unknown Windows layout, FLS slots NOT handled",
                   (unsigned)fd);
    return;
  }
  int chunks = 0;
  for (int k = 0; k < 8; ++k) {
    uintptr_t c = util::ReadOr<uintptr_t>(fd + 8 + k * 4);
    if (!c) continue;
    SIZE_T want = (1 + (16u << k)) * 4;
    if (!HeapValidate(ph, 0, (void*)c) || HeapSize(ph, 0, (void*)c) < want) {
      g_flsWhy = Fmt("FLS: chunk %d at %08X is not a %u B block - unknown layout, FLS slots NOT handled", k,
                     (unsigned)c, (unsigned)want);
      return;
    }
    ++chunks;
  }
  bool found = false;
  g_flsOk = 1;
  ForFls(teb, [&](uint16_t, uintptr_t*, uintptr_t v) { found |= v == me; });
  if (!found) {
    g_flsOk = -1;
    g_flsWhy = "FLS: msvcr100's _ptd not among the slots - unknown layout, FLS slots NOT handled";
    return;
  }
  g_flsWhy = Fmt("FLS layout OK (%d chunks on the game thread, msvcr100 _ptd %08X found)", chunks, (unsigned)me);
  LOG("savestate: %s", g_flsWhy.c_str());
}
// A pointer into the snapshot: a msvcr100 / physics heap segment (also one allocated after a save: an orphan after
// the load) or an exe VirtualAlloc region. Any thread, also while paused (VirtualQuery only).
bool InSnapshotMem(uintptr_t v) {
  if (v < 0x10000) return false;
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery((void*)v, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || mbi.Type != MEM_PRIVATE) return false;
  uintptr_t ab = (uintptr_t)mbi.AllocationBase;
  if (std::binary_search(g_crtSegs.begin(), g_crtSegs.end(), ab) ||
      std::binary_search(g_orphans.begin(), g_orphans.end(), ab))
    return true;
  uintptr_t c = VaCallerNoLock(ab);
  return c && c >= g_exeBase && c < g_exeEnd;
}

void UpdateQuarantine() {
  g_quarantine = g_armed && AnySlotValid();
  // #30: the heap blocks the valid slots hold (hkNtFreeVM keeps their addresses). Not while the game is paused.
  Spin vl(&g_vaLock);
  HzSpin hl;
  int n = 0;
  for (const Slot& sl : g_slots)
    if (sl.valid)
      for (const AllocRec& a : sl.allocs)
        if (a.kind == 2 && n < kHzMax) g_hzKnown[n++] = a.base;
  std::sort(g_hzKnown, g_hzKnown + n);
  g_hzKnownN = (int)(std::unique(g_hzKnown, g_hzKnown + n) - g_hzKnown);
}

// UE3's FFrameEndSync (static in the exe): FEvent* Event[2]; int EventIndex. Each frame the game resets one event,
// queues its trigger for the rendering thread and waits for the other one. The index is in memory (restored), the
// event state is in the kernel (not): a load after an odd number of frames would wait forever. So the state of both
// events is saved and put back. FEventWin = {vtable, HANDLE}.
constexpr const char* kSigFrameEndSync = "50 B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 FF 74 10";
uintptr_t g_frameEndSync = 0;
using NtQueryEvent_t = LONG(NTAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
NtQueryEvent_t g_ntQueryEvent = nullptr;

int GameEvents(HANDLE* out) {  // reads the restored/live memory, no heap
  if (!g_frameEndSync) return 0;
  int n = 0;
  for (int i = 0; i < 2; ++i) {
    uintptr_t ev = 0, h = 0;
    if (util::Read(g_frameEndSync + i * 4, ev) && ev && util::Read(ev + 4, h) && h) out[n++] = (HANDLE)h;
  }
  return n;
}

LONG EventState(HANDLE h) {
  struct {
    int type;
    LONG state;
  } info{};
  return g_ntQueryEvent && g_ntQueryEvent(h, 0, &info, sizeof info, nullptr) >= 0 ? info.state : -1;
}  // allocation bases of msvcr100 heap segments (HeapWalk before the pause)

std::mutex g_msgMu;
std::string g_msg = "off";
// #29: what the player is told (toast) about the request being handled: "Saved to slot 2", "Can't save during a
// cutscene". Set next to every technical return of DoSave / DoLoad (game thread).
std::string g_say;
bool g_sayOk = false;
std::string Say(bool ok, const std::string& player, const std::string& tech) {
  g_say = player;
  g_sayOk = ok;
  return tech;
}
std::atomic<int> g_done{0};
std::atomic<int> g_reqOp{0}, g_reqSlot{0};  // 1 save, 2 load, 3 empty (#27)
std::atomic<DWORD> g_reqAt{0};              // when it was asked (the streaming wait)
std::atomic<bool> g_reqForce{false};        // tests: no streaming wait
int g_activeSlot = 0;
std::vector<uintptr_t> g_freeList;

void SetMsg(const std::string& m) {
  std::lock_guard<std::mutex> lk(g_msgMu);
  g_msg = m;
}

// A 64 MB window into a slot's section (unmapped when it goes out of scope).
struct View {
  uint8_t* p = nullptr;
  uint64_t off = ~0ull, len = 0;
  ~View() {
    if (p) UnmapViewOfFile(p);
  }
};

bool CopySection(View& v, const Slot& sl, uint64_t off, void* mem, size_t size, bool toSection, unsigned* dirty) {
  constexpr uint64_t kView = 64ull << 20;
  uint8_t* p = (uint8_t*)mem;
  while (size) {
    uint64_t vo = off & ~(kView - 1);
    if (v.p && v.off != vo) {
      UnmapViewOfFile(v.p);
      v.p = nullptr;
    }
    if (!v.p) {
      v.len = std::min<uint64_t>(kView, sl.secSize - vo);
      v.p = (uint8_t*)MapViewOfFile(sl.sec, FILE_MAP_WRITE | FILE_MAP_READ, (DWORD)(vo >> 32), (DWORD)vo, (SIZE_T)v.len);
      if (!v.p) return false;
      v.off = vo;
    }
    uint8_t* view = v.p;
    size_t in = (size_t)(off - vo);
    if (in >= v.len) return false;
    size_t n = std::min<size_t>(size, (size_t)(v.len - in));
    if (toSection) {
      memcpy(view + in, p, n);
    } else {
      for (size_t k = 0; k < n; k += 4096) {
        size_t m = std::min<size_t>(4096, n - k);
        if (memcmp(p + k, view + in + k, m) != 0) {
          memcpy(p + k, view + in + k, m);
          ++*dirty;
        }
      }
    }
    p += n;
    off += n;
    size -= n;
  }
  return true;
}

bool IsCrtSeg(uintptr_t ab) { return std::binary_search(g_crtSegs.begin(), g_crtSegs.end(), ab); }

// Allocation bases of msvcr100's heap: its segments and big blocks (HeapWalk, before a pause).
std::vector<uintptr_t> g_heapSet;  // the heaps taken into a slot (handles)

// Heaps of the game: msvcr100's (the exe's malloc) and, with kIncGameDlls, the physics dlls' own: msvcr90's (PhysX
// core) and the static-CRT heaps of the other physics dlls, found as a heap handle stored in their writable data.
void FindHeaps(unsigned inc) {
  g_heapSet.clear();
  uintptr_t crt = 0;
  std::vector<uintptr_t> all = Heaps(&crt);
  uintptr_t proc = (uintptr_t)GetProcessHeap();
  if ((inc & kIncCrtHeap) && crt) g_heapSet.push_back(crt);
  if (!(inc & kIncGameDlls)) return;
  for (const ModRange& m : g_mods)  // msvcr90 comes from WinSxS: GetModuleHandle by name does not find it
    if (!_stricmp(m.name.c_str(), "msvcr90.dll"))
      if (auto f = (intptr_t(__cdecl*)())GetProcAddress((HMODULE)m.base, "_get_heap_handle"))
        if (uintptr_t h = (uintptr_t)f(); h && h != proc) g_heapSet.push_back(h);
  for (const ModRange& m : g_mods) {
    if (!m.phys || !_stricmp(m.name.c_str(), "msvcr90.dll")) continue;
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t a = m.base; a < m.end && VirtualQuery((void*)a, &mbi, sizeof mbi);) {
      uintptr_t n = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
      if (mbi.State == MEM_COMMIT && Writable(mbi.Protect))
        for (uintptr_t* p = (uintptr_t*)a; p < (uintptr_t*)n; ++p) {
          uintptr_t v = *p;
          if (v == proc || v == crt || v < 0x10000 || (v & 0xFFFF)) continue;  // heaps start on 64 KB
          if (std::find(all.begin(), all.end(), v) != all.end() &&
              std::find(g_heapSet.begin(), g_heapSet.end(), v) == g_heapSet.end())
            g_heapSet.push_back(v);
        }
      if (n <= a) break;
      a = n;
    }
  }
}

void CrtSegments() {
  g_crtSegs.clear();
  for (uintptr_t crt : g_heapSet) {
    if (!HeapLock((HANDLE)crt)) continue;
    PROCESS_HEAP_ENTRY e{};
    uintptr_t lb = 0, le = 0;
    while (HeapWalk((HANDLE)crt, &e)) {
      uintptr_t a = (uintptr_t)e.lpData;
      if (a >= lb && a < le) continue;
      MEMORY_BASIC_INFORMATION mbi;
      if (!VirtualQuery((void*)a, &mbi, sizeof mbi)) continue;
      lb = (uintptr_t)mbi.AllocationBase;
      le = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
      g_crtSegs.push_back(lb);
    }
    HeapUnlock((HANDLE)crt);
    g_crtSegs.push_back(crt);
  }
  std::sort(g_crtSegs.begin(), g_crtSegs.end());
  g_crtSegs.erase(std::unique(g_crtSegs.begin(), g_crtSegs.end()), g_crtSegs.end());
}

// The game's own "busy" flag (the one LiveSplit reads, see timers.cpp): 1 = loading screen, 3 = writing the save.
// Nothing may be saved or loaded then (user: "you press the save state before the game has loaded").
constexpr const char* kSigLoadingFlag =
    "A1 ?? ?? ?? ?? 85 C0 74 ?? 56 57 8D 70 0C 56 8B F8 FF 15 ?? ?? ?? ?? 85 C0 75 ?? 56 FF 15 ?? ?? ?? ?? 83 BF ?? ?? ?? "
    "?? 00 74 ?? F6 47 24 80";
uintptr_t g_loadGlobal = 0;
int g_loadOff = 0;

int GameBusy() {
  if (!g_loadGlobal) {
    if (uintptr_t hit = ue3::SigScan(kSigLoadingFlag)) {
      g_loadGlobal = util::ReadOr<uintptr_t>(hit + 1);
      g_loadOff = (int)util::ReadOr<uint32_t>(hit + 36);
    }
    if (!g_loadGlobal) return 0;
  }
  uintptr_t g = util::ReadOr<uintptr_t>(g_loadGlobal);
  if (!g) return 0;
  int v = util::ReadOr<int>(g + g_loadOff);
  return v != 0 && !(util::ReadOr<uint8_t>(g + 0x24) & 0x80) ? v : 0;
}

// Why the game cannot be saved/loaded now ("" = it can). Game thread.
std::string NotNow(const game::World& w, const char* what) {
  const bool save = what[0] == 's';
  const std::string cant = save ? "Can't save" : "Can't load";
  if (!w.mission) {
    nav::Snapshot ns = nav::Now();
    return Say(false,
               cant + (ns.frontEnd ? " in the main menu"
                       : ns.busy || GameBusy() || !ns.ready ? " while the game is loading"
                                                           : " now - only in a mission"),
               std::string(what) + ": not in a mission");
  }
  if (save && !OpenVideos(true, true).empty()) {  // a full-screen video of this map plays: wait (retry, up to 5 s)
    g_retry = true;
    return Say(false, "Can't save during a video", "save: a video is playing - wait until it ends");
  }
  if (nav::Now().busy) return Say(false, cant + " while the level is loading", std::string(what) + ": a load is running");
  if (int b = GameBusy())
    return Say(false, cant + (b == 3 ? " while the game is saving" : " while the game is loading"),
               Fmt("%s: the game is %s - wait", what, b == 3 ? "saving" : "loading"));
  // #29: the game's own pause (pause menu, items, the weapon window): its menus are not part of a slot. The tool's
  // menu pause is fine (taken off after a load: features::AfterStateLoad).
  if (w.world && game::GetObj(w.world, "Pauser") && !features::MenuPauseOn())
    return Say(false, cant + " while the game is paused", std::string(what) + ": the game is paused");
  if (timers::ResultsScreen())
    return Say(false, cant + " on the results screen", std::string(what) + ": the results screen is open");
  if (timers::MissionStartScreen())  // #30: "MISSION 6 ... START MISSION" (a menu over the loaded map)
    return Say(false, cant + " on the mission start screen", std::string(what) + ": the mission start screen is open");
  if (save && w.pc && game::GetBool(w.pc, "bCinematicMode"))
    return Say(false, "Can't save during a cutscene", "save: a cutscene is playing");
  if (save && w.pawn && game::GetFloat(w.pawn, "m_Health", 1) <= 0)
    return Say(false, "Can't save: Dante is dead", "save: Dante is dead");
  return "";
}

// The streaming levels now (game thread).
std::vector<uintptr_t> LoadedLevels(const game::World& w, bool* pending) {
  std::vector<uintptr_t> out;
  *pending = false;
  if (!w.world) return out;
  game::Field f = game::FieldOf(w.world, "StreamingLevels");
  ue3::TArrayRaw a{};
  if (!f.ok() || !util::Read(w.world + f.off, a)) return out;
  for (int i = 0; i < a.num && i < 512; ++i) {
    uintptr_t ls = util::ReadOr<uintptr_t>(a.data + i * 4);
    if (!ue3::IsObject(ls)) continue;
    if (game::GetBool(ls, "bHasLoadRequestPending")) *pending = true;
    uintptr_t lvl = game::GetObj(ls, "LoadedLevel");
    if (lvl) out.push_back(lvl | (game::GetBool(ls, "bIsVisible") ? 1 : 0));
  }
  std::sort(out.begin(), out.end());
  return out;
}

// Streaming is moving (#26): a part is being loaded, made visible/hidden or unloaded. Save/load wait for it (the
// restored memory would hold a half-done step). *what = the first such part. Game thread.
bool StreamingBusy(const game::World& w, std::string* what) {
  if (!w.world) return false;
  game::Field f = game::FieldOf(w.world, "StreamingLevels");
  ue3::TArrayRaw a{};
  if (!f.ok() || !util::Read(w.world + f.off, a)) return false;
  for (int i = 0; i < a.num && i < 512; ++i) {
    uintptr_t ls = util::ReadOr<uintptr_t>(a.data + i * 4);
    if (!ue3::IsObject(ls)) continue;
    uintptr_t lvl = game::GetObj(ls, "LoadedLevel");
    bool load = game::GetBool(ls, "bShouldBeLoaded"), vis = game::GetBool(ls, "bShouldBeVisible"),
         isVis = game::GetBool(ls, "bIsVisible");
    const char* why = nullptr;
    if (game::GetBool(ls, "bHasLoadRequestPending")) why = "loading";
    else if (game::GetBool(ls, "bHasUnloadRequestPending")) why = "unloading";
    else if (load && !lvl) why = "to load";
    else if (!load && lvl) why = "to unload";
    else if (lvl && vis != isVis) why = vis ? "showing" : "hiding";
    if (why) {
      if (what) *what = game::GetName(ls, "PackageName") + " " + why;
      return true;
    }
  }
  return false;
}

// Loaded parts by name ("*" = visible), for the log.
std::string LevelNames(const game::World& w) {
  std::vector<std::string> v;
  if (!w.world) return "-";
  game::Field f = game::FieldOf(w.world, "StreamingLevels");
  ue3::TArrayRaw a{};
  if (!f.ok() || !util::Read(w.world + f.off, a)) return "-";
  for (int i = 0; i < a.num && i < 512; ++i) {
    uintptr_t ls = util::ReadOr<uintptr_t>(a.data + i * 4);
    if (!ue3::IsObject(ls) || !game::GetObj(ls, "LoadedLevel")) continue;
    v.push_back(game::GetName(ls, "PackageName") + (game::GetBool(ls, "bIsVisible") ? "*" : ""));
  }
  std::sort(v.begin(), v.end());
  std::string s;
  for (auto& n : v) s += (s.empty() ? "" : " ") + n;
  return s;
}

void DantePos(const game::World& w, float* pos, int* hp) {
  pos[0] = pos[1] = pos[2] = 0;
  *hp = 0;
  if (!w.pawn) return;
  game::Field f = game::FieldOf(w.pawn, "Location");
  if (f.ok()) util::SafeRead(w.pawn + f.off, pos, 12);
  *hp = (int)game::GetFloat(w.pawn, "m_Health");
}

std::string Fmt(const char* f, ...);
// Dante and the enemies right now (game thread): for the before/after comparison in the log.
std::string StateLine() {
  const game::World& w = game::W();
  if (!w.pawn) return "no player";
  auto vec = [](uintptr_t o, const char* f) {
    float v[3] = {0, 0, 0};
    game::Field fl = game::FieldOf(o, f);
    if (fl.ok()) util::SafeRead(o + fl.off, v, 12);
    return Fmt("%.1f %.1f %.1f", v[0], v[1], v[2]);
  };
  std::string out = Fmt("Dante loc %s vel %s phys %u HP %.0f DT %.1f", vec(w.pawn, "Location").c_str(),
                        vec(w.pawn, "Velocity").c_str(), game::GetByte(w.pawn, "Physics"),
                        game::GetFloat(w.pawn, "m_Health"), game::GetFloat(w.pawn, "m_DevilTriggerGauge"));
  for (uintptr_t e : game::Pawns())
    out += Fmt(" | %s loc %s HP %.0f", ue3::ObjName(e).c_str(), vec(e, "Location").c_str(),
               game::GetFloat(e, "m_Health"));
  return out;
}

std::atomic<bool> g_autoShots{false};
std::atomic<int> g_shotN{0};

std::string Fmt(const char* f, ...) {
  char b[1024];
  va_list a;
  va_start(a, f);
  vsnprintf(b, sizeof b, f, a);
  va_end(a);
  return b;
}

// #27: msvcr100's own atexit table (the exe's atexit() forwards to msvcr100!_onexit): encoded begin/end pointers in
// msvcr100's .data, the table itself in msvcr100's heap. A heap rolled back without that .data = the table pointers
// name a block that is free (or too small) in the restored heap -> the next atexit() reallocs it: C0000374 (crash of
// #26, render thread, a function-local static while a level part streams in); at exit msvcr100 calls garbage.
// Found from msvcr100!_onexit: the callee that does `push [begin]; call DecodePointer; push [end]`.
uintptr_t* g_onexitBegin = nullptr;
uintptr_t* g_onexitEnd = nullptr;
bool FindOnexit() {
  if (g_onexitBegin) return true;
  HMODULE crt = GetModuleHandleW(L"msvcr100.dll");
  auto f = crt ? (const uint8_t*)GetProcAddress(crt, "_onexit") : nullptr;
  if (!f) return false;
  MODULEINFO mi{};
  GetModuleInformation(GetCurrentProcess(), crt, &mi, sizeof mi);
  uintptr_t lo = (uintptr_t)mi.lpBaseOfDll, hi = lo + mi.SizeOfImage;
  __try {
    for (int i = 0; i < 0x40; ++i) {
      if (f[i] != 0xE8) continue;
      const uint8_t* g = f + i + 5 + *(const int32_t*)(f + i + 1);
      if ((uintptr_t)g < lo || (uintptr_t)g >= hi) continue;
      uintptr_t got[2] = {};
      int n = 0;
      for (int k = 0; k < 0x30 && n < 2; ++k)
        if (g[k] == 0xFF && g[k + 1] == 0x35) {
          uintptr_t a = *(const uintptr_t*)(g + k + 2);
          if (a >= lo && a < hi) got[n++] = a;
        }
      if (n == 2) {
        g_onexitBegin = (uintptr_t*)got[0];
        g_onexitEnd = (uintptr_t*)got[1];
        return true;
      }
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
  }
  return false;
}
int OnexitCount() {
  if (!g_onexitBegin) return -1;
  uintptr_t b = (uintptr_t)DecodePointer((void*)*g_onexitBegin), e = (uintptr_t)DecodePointer((void*)*g_onexitEnd);
  return b && e >= b ? (int)((e - b) / 4) : -1;
}
// "atexit table N entries, block OK" / "... BAD ..." (game thread or any time; HeapSize on msvcr100's heap).
std::string OnexitCheck(bool* ok = nullptr) {
  if (ok) *ok = true;
  if (!FindOnexit()) return "atexit table: not found";
  uintptr_t b = (uintptr_t)DecodePointer((void*)*g_onexitBegin), e = (uintptr_t)DecodePointer((void*)*g_onexitEnd);
  uintptr_t crtHeap = 0;
  Heaps(&crtHeap);
  if (!b || !crtHeap) return Fmt("atexit table: empty (begin %08X)", (unsigned)b);
  // HeapSize on a block that is not live = heap corruption fail-fast (C0000374): validate the block first
  SIZE_T sz = HeapValidate((HANDLE)crtHeap, 0, (void*)b) ? HeapSize((HANDLE)crtHeap, 0, (void*)b) : (SIZE_T)-1;
  bool good = sz != (SIZE_T)-1 && e >= b && e - b <= sz;
  if (ok) *ok = good;
  return Fmt("atexit table: %u entries at %08X, heap block %s (%ld B)", (unsigned)((e - b) / 4), (unsigned)b,
             good ? "OK" : "BAD - not a live block of that size", sz == (SIZE_T)-1 ? -1L : (long)sz);
}

std::string ProcMem() {
  PROCESS_MEMORY_COUNTERS_EX pmc{sizeof pmc};
  GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof pmc);
  MEMORY_BASIC_INFORMATION mbi;
  unsigned long long freeVa = 0, biggest = 0;
  for (uintptr_t a = 0x10000; a < 0xFFFF0000 && VirtualQuery((void*)a, &mbi, sizeof mbi);) {
    if (mbi.State == MEM_FREE) {
      freeVa += mbi.RegionSize;
      biggest = std::max<unsigned long long>(biggest, mbi.RegionSize);
    }
    uintptr_t n = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    if (n <= a) break;
    a = n;
  }
  return Fmt("private %.1f MB, working set %.1f MB, free address space %.1f MB (largest %.1f MB), pins %zu",
             pmc.PrivateUsage / 1048576.0, pmc.WorkingSetSize / 1048576.0, freeVa / 1048576.0, biggest / 1048576.0,
             g_pins.size());
}

std::string g_dropWhy[kSlots];  // why a slot became empty by itself (shown with "empty")

// ---- #29: the Save states tab: what each slot shows, its picture, the memory line; the store of a slot's copy
std::mutex g_viewMu;
SlotView g_views[kSlots];        // under g_viewMu (render thread reads copies)
MemView g_mem;                   // under g_viewMu
std::atomic<int> g_thumbWant{-1};  // slot whose picture the next Present takes
struct ThumbRec {
  std::vector<uint8_t> bgra;
  int w = 0, h = 0;
  unsigned gen = 0;
};
ThumbRec g_thumbs[kSlots];  // under g_viewMu
unsigned g_thumbGen = 0;
std::atomic<bool> g_storeGc{false};   // slots were emptied off the game thread: free their copies (WatchSlots)
std::vector<ssstore::Block> g_blocks;  // a packed load's pages (built before the pause)
// #29: what a slot of a new map opening is expected to keep once the map is opened again (address space, MB).
// Measured with "ss mem" around Restart (docs/savestates.md #29); the save is refused when it would not fit.
unsigned long long g_openingCostMB = 400;  // M02, 3 openings: 426 / 417 / 340 MB

void Toast(bool ok, const std::string& text) {
  LOG("savestate: the player is told: %s", text.c_str());
  menu::Toast(text);
  menu::Notice(text, ok);
}

void DanteGauges(const game::World& w, float* hpMax, float* dt, float* dtMax) {
  *hpMax = *dt = *dtMax = 0;
  if (!w.pawn) return;
  *hpMax = game::GetFloat(w.pawn, "m_MaxHealth");
  if (uintptr_t tc = game::GetObj(w.pawn, "m_TriggerComponent")) {
    *dt = game::GetFloat(tc, "m_CurrentTriggerPoints");
    *dtMax = game::GetFloat(tc, "m_MaxTriggerPoints");
  }
}

// Game thread: the tab's copy of slot i (and its picture when it is gone).
void SlotChanged(int i, bool valid) {
  const Slot& sl = g_slots[i];
  std::lock_guard<std::mutex> lk(g_viewMu);
  SlotView& v = g_views[i];
  v = {};
  v.valid = valid && sl.valid;
  v.dropWhy = g_dropWhy[i];
  if (v.valid) {
    v.map = sl.map;
    v.checkpoint = sl.checkpoint;
    v.hour = sl.when.wHour;
    v.minute = sl.when.wMinute;
    memcpy(v.pos, sl.pos, sizeof v.pos);
    v.hp = (float)sl.hp;
    v.hpMax = sl.hpMax;
    v.dt = sl.dt;
    v.dtMax = sl.dtMax;
    v.packed = sl.packed;
    v.loads = sl.loads;
    v.thumbGen = g_thumbs[i].gen;
  } else if (!g_thumbs[i].bgra.empty()) {
    g_thumbs[i] = {};
    g_thumbs[i].gen = ++g_thumbGen;
  }
}

// The slot's copy goes back (game thread, never while paused).
void FreeStore(Slot& sl) {
  std::lock_guard<std::recursive_mutex> sk(ssstore::Mu());
  ++sl.gen;
  ssstore::Release(sl.pages);
  sl.packed = false;
  if (sl.sec) CloseHandle(sl.sec);
  sl.sec = nullptr;
  sl.secSize = 0;
}

// After a save: the packer packs the raw copy; when done the slot drops it (if it is still that copy).
void QueuePack(int s) {
  Slot& sl = g_slots[s];
  HANDLE dup = nullptr;
  if (!ssstore::Enabled() || !sl.sec ||
      !DuplicateHandle(GetCurrentProcess(), sl.sec, GetCurrentProcess(), &dup, 0, FALSE, DUPLICATE_SAME_ACCESS))
    return;
  uint32_t gen = sl.gen;
  ssstore::Pack(dup, sl.used / 4096,
                [s, gen](ssstore::PageList&& pl) {  // packer thread, ssstore::Mu() held
                  Slot& x = g_slots[s];
                  if (x.gen != gen || !x.valid) return false;
                  x.pages = std::move(pl);
                  x.packed = true;
                  if (x.sec) CloseHandle(x.sec);
                  x.sec = nullptr;
                  x.secSize = 0;
                  std::lock_guard<std::mutex> lk(g_viewMu);
                  g_views[s].packed = true;
                  return true;
                },
                Fmt("slot %d", s + 1));
}

void FreeVa(unsigned long long* total, unsigned long long* biggest);
// The memory line of the tab (game thread, once a second).
void UpdateMemView(const game::World& w) {
  MemView m;
  unsigned long long tot = 0, big = 0;
  FreeVa(&tot, &big);
  m.freeBytes = tot;
  ssstore::Stats st = ssstore::GetStats();
  m.storeBytes = st.chunkBytes;
  {
    std::lock_guard<std::recursive_mutex> sk(ssstore::Mu());
    for (const Slot& x : g_slots) m.rawBytes += x.sec ? x.secSize : 0;
  }
  {  // address space the slots keep that the game does not use now: allocations it released (zombies), orphans
    Spin lk(&g_vaLock);
    for (size_t i = 0; i < kVaSlots; ++i)
      if (g_va[i].base > 1 && g_va[i].zombie) {
        MEMORY_BASIC_INFORMATION mbi;
        uintptr_t b = g_va[i].base;
        for (uintptr_t a = b; VirtualQuery((void*)a, &mbi, sizeof mbi) && (uintptr_t)mbi.AllocationBase == b &&
                              mbi.State != MEM_FREE;
             a += mbi.RegionSize)
          m.heldBytes += mbi.RegionSize;
      }
    for (uintptr_t b : g_orphans) {
      MEMORY_BASIC_INFORMATION mbi;
      for (uintptr_t a = b; VirtualQuery((void*)a, &mbi, sizeof mbi) && (uintptr_t)mbi.AllocationBase == b &&
                            mbi.State != MEM_FREE;
           a += mbi.RegionSize)
        m.heldBytes += mbi.RegionSize;
    }
  }
  int openings = 0;
  for (int i = 0; i < kSlots; ++i) {
    if (!g_slots[i].valid || g_slots[i].world == w.world) continue;
    bool first = true;
    for (int k = 0; k < i; ++k)
      if (g_slots[k].valid && g_slots[k].world == g_slots[i].world) first = false;
    openings += first;
  }
  m.oldOpenings = openings;
  m.packing = ssstore::Packing();
  std::lock_guard<std::mutex> lk(g_viewMu);
  g_mem = m;
}

// #27: the rendering thread. A loading screen / transition (M02: the bedroom -> "dream") stops the game's rendering
// thread and starts a new one (UE3 StopRenderingThread / StartRenderingThread): a slot saved before it points at the
// old thread (its FRunnableThread object, the runnable, its stack). A load then: StopRenderingThread() (the game's
// own, before the pause), memory back, in the restored memory "no rendering thread" (GIsThreadedRendering = 0,
// GRenderingThread = GRenderingThreadRunnable = NULL; the old objects stay allocated), after the pause the game's
// StartRenderingThread() - a new thread runs the restored command queue (empty: a save waits for its rest).
// v1.3: Start RVA 0x2762A0, Stop 0x27A120; v1.0: 0x276E70 / 0x27AD60. Globals read from both and cross-checked.
constexpr const char* kSigStartRT =
    "6A 08 6A 04 C7 05 ?? ?? ?? ?? 01 00 00 00 E8 ?? ?? ?? ?? 83 C4 08 85 C0 74 ?? C7 00 ?? ?? ?? ?? A3";
constexpr const char* kSigStopRT =
    "56 33 F6 39 35 ?? ?? ?? ?? 0F 84 ?? ?? ?? ?? 39 35 ?? ?? ?? ?? 74 ?? FF 15 ?? ?? ?? ?? 3B 05 ?? ?? ?? ?? 0F 85 ?? "
    "?? ?? ?? 57 8B 3D";
using VoidFn_t = void(__cdecl*)();
VoidFn_t g_startRT = nullptr, g_stopRT = nullptr;
uint32_t* g_gIsThreadedRendering = nullptr;
uintptr_t* g_gRenderingThread = nullptr;       // FRunnableThread*
uintptr_t* g_gRenderingThreadRunnable = nullptr;  // FRenderingThread*
std::atomic<long> g_rtRestarts{0};

void FindRenderThreadFns() {
  uintptr_t a = ue3::SigScan(kSigStartRT), b = ue3::SigScan(kSigStopRT);
  if (!a || !b) {
    LOG("savestate: Start/StopRenderingThread not found (%p %p) - a slot from before a loading screen cannot load",
        (void*)a, (void*)b);
    return;
  }
  uint32_t itr = *(uint32_t*)(a + 6), runnable = *(uint32_t*)(a + 0x21), rt = *(uint32_t*)(a + 0x62);
  bool ok = *(uint8_t*)(a + 0x61) == 0xA3 && *(uint32_t*)(b + 5) == itr && *(uint16_t*)(b + 0x49) == 0x0D8B &&
            *(uint32_t*)(b + 0x4B) == rt && *(uint16_t*)(b + 0x4F) == 0x3589 && *(uint32_t*)(b + 0x51) == itr &&
            *(uint16_t*)(b + 0x70) == 0x0D8B && *(uint32_t*)(b + 0x72) == runnable;
  if (!ok) {
    LOG("savestate: Start/StopRenderingThread at RVA 0x%X / 0x%X do not match each other - not used",
        (unsigned)(a - g_exeBase), (unsigned)(b - g_exeBase));
    return;
  }
  g_startRT = (VoidFn_t)a, g_stopRT = (VoidFn_t)b;
  g_gIsThreadedRendering = (uint32_t*)(uintptr_t)itr;
  g_gRenderingThread = (uintptr_t*)(uintptr_t)rt;
  g_gRenderingThreadRunnable = (uintptr_t*)(uintptr_t)runnable;
  LOG("savestate: StartRenderingThread RVA 0x%X, StopRenderingThread RVA 0x%X, GIsThreadedRendering %08X, "
      "GRenderingThread %08X, runnable %08X",
      (unsigned)(a - g_exeBase), (unsigned)(b - g_exeBase), itr, rt, runnable);
}

// The OS thread of GRenderingThread: a field of the FRunnableThreadWin object that is a handle of a live thread of
// ours (no fixed layout needed). 0 = none / not threaded.
DWORD RenderTid() {
  if (!g_gRenderingThread || !g_gIsThreadedRendering || !*g_gIsThreadedRendering) return 0;
  uintptr_t o = *g_gRenderingThread;
  if (!o) return 0;
  for (int i = 1; i < 8; ++i) {
    uintptr_t h = 0;
    if (!util::Read(o + i * 4, h) || !h || (h & 3)) continue;
    DWORD tid = GetThreadId((HANDLE)h);
    if (tid && GetProcessIdOfThread((HANDLE)h) == GetCurrentProcessId()) return tid;
  }
  return 0;
}

void FreeVa(unsigned long long* total, unsigned long long* biggest);
extern unsigned long long g_valveTotalMB, g_valveBiggestMB;

// ---- save (game thread, start of a frame)
std::string DoSave(int s) {
  const game::World& w = game::W();
  if (std::string why = NotNow(w, "save"); !why.empty()) return why;
  bool pending = false;
  std::vector<uintptr_t> levels = LoadedLevels(w, &pending);
  if (pending) return Say(false, "Can't save while the level is loading - try again", "save: a part of the level is loading, try again");
  long long t0 = RealQpc();
  Slot& sl = g_slots[s];
  // estimate + section, before the pause
  unsigned inc = g_inc;
  RefreshModules();
  PatchGameModules();  // #28: waits / closes / TerminateThread of the game's dlls (loaded late: PhysX)
  FindOnexit();  // #27: before the pause (GetProcAddress takes the loader lock)
  FindHeaps(inc);
  CrtSegments();
  ListGameThreads();
  CheckFlsLayout();  // #27
  DWORD renderTid = RenderTid();  // #27
  uintptr_t exeBase = (uintptr_t)GetModuleHandleW(nullptr);
  auto include = [&](const MEMORY_BASIC_INFORMATION& mbi, uint8_t* kind, uintptr_t* caller) -> bool {
    if (mbi.State != MEM_COMMIT || !Writable(mbi.Protect) || (mbi.Protect & PAGE_GUARD)) return false;
    uintptr_t ab = (uintptr_t)mbi.AllocationBase;
    *caller = 0;
    if (mbi.Type == MEM_IMAGE) {
      *kind = 0;
      if (ab == exeBase) return (inc & kIncExeData) != 0;
      const ModRange* m = ModOf(ab);
      return m && (((inc & kIncGameDlls) && m->phys) || ((inc & kIncCrtData) && m->crt));
    }
    if (mbi.Type != MEM_PRIVATE) return false;
    if (uintptr_t c = VaCallerNoLock(ab)) {
      const ModRange* m = ModOf(c);
      *kind = 1;
      *caller = c;
      return (inc & kIncExeVA) && m && m->base == exeBase;
    }
    *kind = 2;
    return IsCrtSeg(ab);  // a heap of g_heapSet
  };
  uint64_t est = 0;
  {
    MEMORY_BASIC_INFORMATION mbi;
    uint8_t k;
    uintptr_t c;
    for (uintptr_t a = 0x10000; a < 0xFFFF0000 && VirtualQuery((void*)a, &mbi, sizeof mbi);) {
      if (InterlockedCompareExchange(&g_vaLock, 1, 0) == 0) {  // include() reads the table without a lock
        if (include(mbi, &k, &c)) est += mbi.RegionSize;
        InterlockedExchange(&g_vaLock, 0);
      }
      uintptr_t n = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
      if (n <= a) break;
      a = n;
    }
  }
  uint64_t want = est + est / 4 + (64ull << 20);
  {  // #27: never run the game out of address space: the slot keeps memory alive (quarantine: freed allocations stay
     // reserved while a slot needs them; a slot of another map keeps that whole map). Refuse before anything changes
     // when the room left would be under the low-memory valve plus a margin for the level to go on streaming.
     // #29 (5 slots): a slot of a map opening no other slot holds will keep that opening once the map is opened
     // again (Restart, checkpoint): that cost is reserved now too (g_openingCostMB, measured - see "ss mem").
     // Overwriting the only slot of an older opening frees one: never refused.
    unsigned long long tot = 0, big = 0;
    FreeVa(&tot, &big);
    bool shared = false, frees = sl.valid && sl.world != w.world;
    for (int i = 0; i < kSlots; ++i) {
      if (i == s || !g_slots[i].valid) continue;
      if (g_slots[i].world == w.world) shared = true;
      if (sl.valid && g_slots[i].world == sl.world) frees = false;
    }
    unsigned long long needTot = (g_valveTotalMB + 300 + (shared ? 0 : g_openingCostMB)) << 20,
                       needBig = (g_valveBiggestMB + 64) << 20;
    if ((tot < needTot || big < needBig) && !frees) {
      LOG("savestate: save slot %d refused: free address space %.0f MB (largest %.0f MB), need %llu / %llu MB "
          "(this map opening %s)", s + 1, tot / 1048576.0, big / 1048576.0, needTot >> 20, needBig >> 20,
          shared ? "held by another slot already" : "new to the slots");
      return Say(false, "Not enough game memory - empty a slot",
                 Fmt("save: the game is low on memory (%.0f MB free) - not saved, the game is unchanged",
                     tot / 1048576.0));
    }
  }
  // #29: the packer must not publish (and close the raw copy) between this check and the copy: the store stays
  // locked until the save is done (the packer only waits; it never holds the lock while the game is paused)
  std::unique_lock<std::recursive_mutex> storeLock(ssstore::Mu());
  if (!sl.sec || sl.secSize < est + (16ull << 20)) {
    if (sl.sec) CloseHandle(sl.sec);
    sl.sec = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, (DWORD)(want >> 32),
                                (DWORD)want, nullptr);
    sl.secSize = sl.sec ? want : 0;
    if (!sl.sec) {  // (the slot's packed copy, if any, is untouched: the slot stays as it was)
      return Say(false, "Not enough memory for a save state - empty a slot",
                 Fmt("save: no memory for the slot (%.0f MB, error %lu)", want / 1048576.0, GetLastError()));
    }
  }
  if (!g_reqForce) {  // #26: every game thread at its rest (see the sampler), else retry next frame - checked in a
                      // short pause of its own, before the slot is touched (a retry keeps the old save)
    int tr0 = 0, bad0 = -1;
    if (!Pause(&tr0, &bad0)) return NotStopped("save", bad0);
    DWORD busyTid = 0;
    uintptr_t busyRet = 0, restRet = 0;
    uint32_t busyDepth = 0, restDepth = 0;
    for (const PThread& t : g_pt) {
      if (!t.suspended || !t.mustIdle || !t.esp) continue;
      if (KeepFindNoLock(t.tid, t.created) >= 0) continue;  // #28: kept (parked / frozen): not part of this save
      uintptr_t rr;
      uint32_t rd;
      if (!RestOf(t.tid, &rr, &rd)) continue;
      uintptr_t er = ExeWaitCaller(t.esp, t.stackBase, g_checkStack);
      uint32_t d = (uint32_t)(t.stackBase - t.esp);
      if (d != rd && !RestOk(t.tid, d)) {
        busyTid = t.tid, busyRet = er, busyDepth = d, restRet = rr, restDepth = rd;
        break;
      }
    }
    Unpause();
    if (busyTid) {
      LOG("savestate: save slot %d: thread %lu %s busy at exe+0x%X depth %u (rest exe+0x%X depth %u)", s + 1, busyTid,
          NameOf(busyTid).c_str(), (unsigned)(busyRet - g_exeBase), busyDepth, (unsigned)(restRet - g_exeBase),
          restDepth);
      g_retry = true;
      return Say(false, "Couldn't save right now - try again",
                 Fmt("save: a game thread is busy (%s) - not saved yet", NameOf(busyTid).c_str()));
    }
  }
  {  // #29: from here the slot's old copy is gone (its packed pages back to the store; a pack job of it is dropped)
    std::lock_guard<std::recursive_mutex> sk(ssstore::Mu());
    ++sl.gen;
    ssstore::Release(sl.pages);
    sl.packed = false;
  }
  SlotChanged(s, false);
  {
    Spin lk(&g_vaLock);  // hkVirtualFree reads the slot's allocations
    sl.valid = false;
    sl.runs.clear();
    sl.runs.reserve(200000);
    sl.allocs.clear();
    sl.allocs.reserve(60000);
    sl.tls.clear();
    sl.tls.reserve(8192);
    sl.flsThreads.clear();
    sl.flsThreads.reserve(g_ptdThreads.size() + 64);
    sl.fls.clear();
    sl.fls.reserve(65536);
    sl.stacks.clear();
    sl.stacks.reserve(512);
    sl.threadHandles.clear();
    sl.threadHandles.reserve(256);
    sl.stackData.clear();
    sl.stackData.reserve(16u << 20);
  }
  GameTlsIndices();
  long long t1 = RealQpc();
  // ---- pause
  int tries = 0, bad = -1;
  if (!Pause(&tries, &bad)) return NotStopped("save", bad);
  for (Pin& p : g_pins) p.mask &= ~(1 << s);
  long long t2 = RealQpc();
  bool ok = true;
  const char* fail = nullptr;
  uint64_t off = 0;
  unsigned long long bytes[3] = {0, 0, 0};
  {
    View view;
    MEMORY_BASIC_INFORMATION mbi;
    uintptr_t curAb = 0;
    for (uintptr_t a = 0x10000; ok && a < 0xFFFF0000 && VirtualQuery((void*)a, &mbi, sizeof mbi);) {
      uintptr_t n = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
      if (n <= a) break;
      uintptr_t ab = (uintptr_t)mbi.AllocationBase;
      if (mbi.State != MEM_FREE && ab == curAb && !sl.allocs.empty() && sl.allocs.back().base == ab)
        sl.allocs.back().extent = n - ab;
      uint8_t kind;
      uintptr_t caller;
      if (include(mbi, &kind, &caller)) {
        if (kind != 0 && (sl.allocs.empty() || sl.allocs.back().base != ab)) {
          if (sl.allocs.size() == sl.allocs.capacity()) { ok = false; fail = "too many allocations"; break; }
          sl.allocs.push_back({ab, n - ab, caller, kind});
          curAb = ab;
        }
        if (sl.runs.size() == sl.runs.capacity()) { ok = false; fail = "too many regions"; break; }
        if (off + mbi.RegionSize > sl.secSize) { ok = false; fail = "slot too small"; break; }
        unsigned d = 0;
        if (!CopySection(view, sl, off, (void*)a, mbi.RegionSize, true, &d)) { ok = false; fail = "map view"; break; }
        sl.runs.push_back({a, (uint32_t)mbi.RegionSize, kind, off});
        bytes[kind] += mbi.RegionSize;
        off += mbi.RegionSize;
      }
      a = n;
    }
  }
  long long t3 = RealQpc();
  int pins = 0, files = 0, hsaved = 0;
  if (ok) {
    for (Pin& p : g_pins) {
      int r = GameRefs(p.obj);
      if (r > 0) {
        p.slotRefs[s] = r;
        p.mask |= 1 << s;
        ++pins;
      }
    }
    files = FilesSaveNoLock(s);
  } else {
    FilesDropSlotNoLock(s);
    HandlesDropSlotNoLock(s);  // #28
  }
  long long virt = RealQpc() - g_qpcOff.load();
  HANDLE evH[4] = {};
  LONG evS[4] = {};
  int nEv = ok ? GameEvents(evH) : 0;
  for (int i = 0; i < nEv; ++i) evS[i] = EventState(evH[i]);
  if (ok) {
    uintptr_t selfTeb = (uintptr_t)NtCurrentTeb();
    auto take = [&](DWORD tid, uintptr_t teb) {
      for (int i : g_gameTls)
        if (uintptr_t* c = TlsCell(teb, i); c && sl.tls.size() < sl.tls.capacity())
          sl.tls.push_back({tid, teb, i, *c});
    };
    take(GetCurrentThreadId(), selfTeb);
    for (PThread& t : g_pt)
      if (t.mustIdle && t.suspended && t.teb) take(t.tid, t.teb);
    auto flsTake = [&](DWORD tid, uint64_t created, uintptr_t teb) {  // #27
      if (sl.flsThreads.size() >= sl.flsThreads.capacity()) return;
      uint32_t b = (uint32_t)sl.fls.size();
      ForFls(teb, [&](uint16_t pos, uintptr_t*, uintptr_t v) {
        if (v && sl.fls.size() < sl.fls.capacity()) sl.fls.push_back({pos, v});
      });
      sl.flsThreads.push_back({tid, created, b, (uint32_t)sl.fls.size()});
    };
    flsTake(GetCurrentThreadId(), g_gameCreated, selfTeb);
    for (const PtdThread& t : g_ptdThreads) flsTake(t.tid, t.created, t.teb);
    auto keep = [&](DWORD tid, uintptr_t from, uintptr_t base, uintptr_t eip, const PThread* t) {
      if (!from || base <= from || base - from > (4u << 20)) return;
      size_t n = base - from, at = sl.stackData.size();
      if (at + n > sl.stackData.capacity()) return;
      sl.stackData.resize(at + n);  // within the capacity: no allocation
      if (!util::SafeRead(from, sl.stackData.data() + at, n)) {
        sl.stackData.resize(at);
        return;
      }
      Slot::StackRec rec{tid, from, eip, base, t ? t->gameRet : 0, (uint32_t)at, (uint32_t)n, t ? t->ebx : 0,
                         t ? t->esi : 0, t ? t->edi : 0, t ? t->ebp : 0, t ? t->start : 0, 0, 0x1F80, 0x27F};
      if (t) {  // #28: what a kept thread needs to go back into this wait
        util::Read(t->teb, rec.seh);
        CONTEXT c{};
        c.ContextFlags = CONTEXT_FLOATING_POINT | CONTEXT_EXTENDED_REGISTERS;
        if (GetThreadContext(t->h, &c)) {
          rec.fcw = (uint16_t)c.FloatSave.ControlWord;
          memcpy(&rec.mxcsr, c.ExtendedRegisters + 24, 4);
        }
      }
      sl.stacks.push_back(rec);
    };
    keep(GetCurrentThreadId(), g_tickFrame, __readfsdword(4), 0, nullptr);
    for (PThread& t : g_pt)
      if (t.mustIdle && t.suspended && t.esp && KeepFindNoLock(t.tid, t.created) < 0) keep(t.tid, t.esp, t.stackBase, t.eip, &t);
    EnumHandlesNoLock();  // #28: kernel objects open at this save (g_enumLock held by Pause)
    hsaved = HandlesSaveNoLock(s);
    sl.threadHandles.clear();  // handle values naming the game's threads (reserved before the pause)
    for (int i = 0; i < g_hentN && sl.threadHandles.size() < sl.threadHandles.capacity(); ++i)
      if (g_hents[i].type == g_tyThread)
        if (DWORD tid = GetThreadId(g_hents[i].h))
          for (const PThread& t : g_pt)
            if (t.tid == tid && t.mustIdle && t.h != g_hents[i].h) sl.threadHandles.push_back({g_hents[i].h, tid, t.created});
  }
  sl.onexitN = OnexitCount();  // #27 (frozen: only reads + DecodePointer)
  sl.valid = ok;  // under g_vaLock: from now the quarantine keeps this slot's allocations
  if (ok) g_dropWhy[s].clear();
  int zr = ReleaseZombiesNoLock();
  zr += PruneOrphansNoLock();
  Unpause();
  UpdateQuarantine();
  // ---- resumed
  long long t4 = RealQpc();
  if (!ok) return Say(false, "Save failed", Fmt("save: failed (%s)", fail));
  sl.used = off;
  sl.map = w.map;
  sl.levels = levels;
  sl.levelNames = LevelNames(w);
  sl.world = w.world;
  sl.videos = OpenVideos(false, true);
  VideosSave(s);  // #29: in-world videos open now are kept for this slot
  sl.threads.clear();
  int keptNow = 0;
  for (const PThread& t : g_pt)
    if (t.mustIdle) {
      if (KeepFindNoLock(t.tid, t.created) >= 0) {
        ++keptNow;
        continue;
      }
      sl.threads.push_back({t.tid, t.created, t.start});
    }
  std::sort(sl.threads.begin(), sl.threads.end());
  sl.renderTid = renderTid;
  sl.files = files;
  sl.handles = hsaved;
  RebuildKnown();  // #28
  int rel = ReleaseKept();
  LOG("savestate: slot %d: %zu game threads, %d kernel objects (events/semaphores/mutexes/threads) known; %d kept "
      "threads not part of it, %d released (no slot needs them)",
      s + 1, sl.threads.size(), hsaved, keptNow, rel);
  LOG("savestate: slot %d level parts at the save (%zu): %s", s + 1, levels.size(), sl.levelNames.c_str());
  {
    std::vector<Module> mods = Modules();
    std::string d;
    for (const Slot::StackRec& r : sl.stacks)
      if (r.gameRet) d += Fmt(" %s@%s/%u", NameOf(r.tid).c_str(), Where(mods, r.gameRet).c_str(), (unsigned)(r.base - r.esp));
    LOG("savestate: slot %d thread waits:%s", s + 1, d.c_str());
  }
  sl.qpcVirtual = virt;
  DantePos(w, sl.pos, &sl.hp);
  sl.pauseMs = QpcMs(t2, t4);
  sl.copyMs = QpcMs(t2, t3);
  sl.totalMs = QpcMs(t0, t4);
  sl.pauseTries = tries;
  sl.loads = 0;
  memcpy(sl.bytes, bytes, sizeof bytes);
  sl.pins = pins;
  sl.nEv = nEv;
  sl.heaps = g_heapSet;
  {
    std::string hs;
    for (uintptr_t h : g_heapSet) hs += Fmt(" %08X", (unsigned)h);
    LOG("savestate: heaps taken:%s; game TLS indices %zu, values %zu", hs.c_str(), g_gameTls.size(), sl.tls.size());
  }
  memcpy(sl.evH, evH, sizeof evH);
  memcpy(sl.evS, evS, sizeof evS);
  GetLocalTime(&sl.when);
  DanteGauges(w, &sl.hpMax, &sl.dt, &sl.dtMax);
  {
    uintptr_t cp = w.engine ? game::GetObj(w.engine, "GCheckPointData") : 0;
    sl.checkpoint = cp ? game::GetStr(cp, "m_CheckpointName") : "";
  }
  QueuePack(s);
  SlotChanged(s, true);
  g_thumbWant = s;  // the next presented frame: the slot's picture
  Say(true, Fmt("Saved to slot %d", s + 1), "");
  return Fmt("save slot %d: %.1f MB (images %.1f, exe VA %.1f, heaps %.1f [%zu]) in %zu regions / %zu allocations, "
             "game stopped %.1f ms (copy %.1f, %d tries), total %.1f ms (prepare %.1f), %d D3D objects pinned, %d files; "
             "%s, %zu level parts, Dante %.0f %.0f %.0f HP %d; zombies released %d; %s",
             s + 1, off / 1048576.0, bytes[0] / 1048576.0, bytes[1] / 1048576.0, bytes[2] / 1048576.0, g_heapSet.size(),
             sl.runs.size(), sl.allocs.size(), sl.pauseMs, sl.copyMs, tries, sl.totalMs, QpcMs(t0, t1), pins, files,
             w.map.c_str(), levels.size(), sl.pos[0], sl.pos[1], sl.pos[2], sl.hp, zr, ProcMem().c_str());
}

// ---- load (game thread, start of a frame)
void DropSlot(int i, const char* why);
std::string DoLoad(int s) {
  Slot& sl = g_slots[s];
  if (!sl.valid)
    return Say(false, Fmt("Slot %d is empty", s + 1),
               g_dropWhy[s].empty() ? Fmt("load: slot %d is empty", s + 1)
                                    : Fmt("load: slot %d is empty (%s)", s + 1, g_dropWhy[s].c_str()));
  const game::World& w = game::W();
  if (std::string why = NotNow(w, "load"); !why.empty()) return why;
  if (_stricmp(w.map.c_str(), sl.map.c_str()) != 0) {
    int mi = nav::FindMission(sl.map);
    std::string where = mi >= 0 ? nav::Missions()[mi].id : nav::FindDevRoom(sl.map) >= 0 ? "Dev room" : sl.map;
    return Say(false, Fmt("Slot %d is from another mission (%s) - not loaded", s + 1, where.c_str()),
               Fmt("load: slot %d is from another map (%s, now %s) - not loaded, the slot is kept", s + 1,
                   sl.map.c_str(), w.map.c_str()));
  }
  if (OpenVideos(false, true) != sl.videos) {  // #26: a full-screen video opened (or closed) since the save
    // #27: stop it the game's way - Engine.StopMovie(false) (force, also an unskippable one: M02's transition into
    // the "dream" is a Bink video EyeIn_1.bik) - and load next frame when it is closed (BinkClose seen).
    static DWORD lastStop = 0;
    if (w.engine && GetTickCount() - lastStop > 500) {
      lastStop = GetTickCount();
      engine::Call c(w.engine, "StopMovie");
      bool ok = c.Valid() && c.Bool("bDelayStopUntilGameHasRendered", false).Invoke();
      LOG("savestate: load slot %d: a video opened since the save - Engine.StopMovie %s", s + 1, ok ? "called" : "FAILED");
    }
    g_retry = true;
    return Say(false, "Couldn't load right now - try again", "load: stopping the video that started after the save");
  }
  if (!g_early && w.world != sl.world)  // #28: turned on late: D3D resources of the game made before are not held
    return Say(false, Fmt("Slot %d can't load after the map was loaded again - save again", s + 1), Fmt("load: slot %d - the map was loaded again since the save, and save states were turned on after the game "
               "started (its graphics from before are not protected); turn on dev_savestates in the config and "
               "restart the game. The slot is kept",
               s + 1));
  bool pending = false;
  std::vector<uintptr_t> levels = LoadedLevels(w, &pending);
  // #26: other parts of the level may be loaded now - the slot's memory brings its own parts back (kept alive: their
  // memory in quarantine, D3D9 resources pinned, files open), the parts loaded since vanish with the new memory.
  LOG("savestate: load slot %d: level parts now (%zu): %s%s", s + 1, levels.size(), LevelNames(w).c_str(),
      w.world != sl.world ? " [the map was opened again since the save]" : "");
  long long t0 = RealQpc();
  RefreshModules();
  // #27: the rendering thread was replaced since the save (a loading screen / transition): the game's own
  // StopRenderingThread now; StartRenderingThread when this load returns, whatever happens (RtGuard).
  bool rtRestart = false, rtNone = false;
  if (g_stopRT) {  // #28: also after the map was opened again (the slot's threads are kept now)
    DWORD nowRt = RenderTid();
    // the game may also run without a rendering thread for a while (it stopped it, e.g. around a device reset):
    // then the restored memory just gets "no rendering thread", like the game now
    rtNone = !nowRt && sl.renderTid && g_gIsThreadedRendering && !*g_gIsThreadedRendering;
    if (rtNone)
      LOG("savestate: load slot %d: the game runs without a rendering thread now (%lu at the save)", s + 1,
          sl.renderTid);
    if (nowRt && nowRt != sl.renderTid) {
      LOG("savestate: load slot %d: the rendering thread was replaced since the save (%lu -> %lu): stopping it",
          s + 1, sl.renderTid, nowRt);
      g_stopRT();
      rtRestart = true;
      ++g_rtRestarts;
    }
  }
  struct RtGuard {
    bool on;
    ~RtGuard() {
      if (on && g_startRT && g_gIsThreadedRendering && !*g_gIsThreadedRendering) {
        g_startRT();
        LOG("savestate: rendering thread started again (%lu)", RenderTid());
      }
    }
  } rtGuard{rtRestart};
  PatchGameModules();  // #28
  ListGameThreads();
  CheckFlsLayout();  // #27
  // #28: the threads of the save. The game recreates its physics pools when it opens a map again (Restart, Load
  // checkpoint, another mission): their old threads were kept (parked / frozen, see "kept threads"), the new ones
  // belong to the dropped timeline. Plan: every thread of the save must be alive (parked -> back into its wait,
  // frozen -> resumed, else as it is); every other game thread: kept already -> stays, known to the other slot ->
  // frozen, else -> ended. Same thread = same id + creation time.
  auto findKey = [&](DWORD tid) -> const ThreadKey* {
    auto it = std::lower_bound(sl.threads.begin(), sl.threads.end(), ThreadKey{tid, 0, 0});
    return it != sl.threads.end() && it->tid == tid ? &*it : nullptr;
  };
  std::vector<int> revive, unfreeze;  // g_keep indices
  std::vector<size_t> freeze, kill;   // g_pt indices
  {
    std::string lost;
    for (const ThreadKey& k : sl.threads) {
      bool alive = false;
      for (const PThread& t : g_pt) alive |= t.tid == k.tid && t.created == k.created;
      if (alive) {
        int kr = KeepFindNoLock(k.tid, k.created);
        if (kr >= 0 && g_keep[kr].state == kKeepParked) revive.push_back(kr);
        else if (kr >= 0 && g_keep[kr].state == kKeepFrozen) unfreeze.push_back(kr);
        else if (kr >= 0) lost += Fmt(" %lu %s (kept, state %ld)", k.tid, NameOf(k.tid).c_str(), g_keep[kr].state);
        continue;
      }
      if (k.tid == sl.renderTid && (rtRestart || rtNone)) continue;  // #27: stopped above, a new one after the load
      lost += Fmt(" %lu %s", k.tid, NameOf(k.tid).c_str());
    }
    if (!lost.empty()) {
      LOG("savestate: load slot %d: thread(s) of the save ended:%s", s + 1, lost.c_str());
      DropSlot(s, "a game thread of the save has ended");
      return Say(false, Fmt("Slot %d was lost - save again", s + 1),
                 Fmt("load: slot %d is lost - a game thread of the save has ended (%s); save again", s + 1,
                     lost.c_str() + 1));
    }
    for (size_t i = 0; i < g_pt.size(); ++i) {
      const PThread& t = g_pt[i];
      if (!t.mustIdle) continue;
      const ThreadKey* k = findKey(t.tid);
      if (k && k->created == t.created) continue;  // a thread of the save
      if (KeepFindNoLock(t.tid, t.created) >= 0) continue;  // kept for the other slot already
      uint8_t m;
      {
        Spin lk(&g_fileLock);
        m = KnownMaskNoLock(t.tid, t.created);
      }
      (m & ~(1 << s) ? freeze : kill).push_back(i);
    }
    if (!revive.empty() || !unfreeze.empty() || !freeze.empty() || !kill.empty()) {
      std::string d;
      for (int kr : revive) d += Fmt(" back:%lu %s", g_keep[kr].tid, NameOf(g_keep[kr].tid).c_str());
      for (int kr : unfreeze) d += Fmt(" resume:%lu %s", g_keep[kr].tid, NameOf(g_keep[kr].tid).c_str());
      for (size_t i : freeze) d += Fmt(" freeze:%lu %s", g_pt[i].tid, NameOf(g_pt[i].tid).c_str());
      for (size_t i : kill) d += Fmt(" end:%lu %s", g_pt[i].tid, NameOf(g_pt[i].tid).c_str());
      LOG("savestate: load slot %d: thread plan (%zu of the save):%s", s + 1, sl.threads.size(), d.c_str());
    }
  }
  auto isRevive = [&](DWORD tid) {
    for (int kr : revive)
      if (g_keep[kr].tid == tid) return true;
    return false;
  };
  g_heapSet = sl.heaps;
  CrtSegments();  // the heaps of the save
  uintptr_t exeBase = (uintptr_t)GetModuleHandleW(nullptr);
  g_freeList.clear();
  g_freeList.reserve(65536);
  g_orphanTmp.clear();
  g_orphanTmp.reserve(g_orphans.size() + g_crtSegs.size() + 64);
  g_retry = false;
  // #29: the store stays as it is until this load is over (the packer waits); a packed slot: the blocks of pages to
  // put back, made now (no heap use while the game is paused)
  std::unique_lock<std::recursive_mutex> storeLock(ssstore::Mu());
  const bool packed = sl.packed;
  g_blocks.clear();
  if (packed) {
    ssstore::PrepareRestore(sl.pages.size());
    for (const RunRec& r : sl.runs)
      for (uint32_t k = 0; k < r.size; k += 32 * 4096)
        g_blocks.push_back({r.addr + k, (uint32_t)((r.off + k) / 4096), std::min<uint32_t>(32, (r.size - k) / 4096)});
  }
  long long t1 = RealQpc();
  int tries = 0, bad = -1;
  if (!Pause(&tries, &bad)) return NotStopped("load", bad);
  long long t2 = RealQpc();
  // 0. (#26) every game thread must wait where it waited at the save: its stack goes back only then. A thread that
  // waits elsewhere (a worker inside a streaming task) would wake up with locals of the dropped timeline -> crash
  // (M02: FEvent::Wait on an event the restored memory no longer has). Then: resume, retry next frame (up to 5 s).
  struct Off {
    DWORD tid;
    uintptr_t eipNow, espNow, eipSave, espSave, base;
  } offs[8];  // eip = the wait's caller in game code
  int nOff = 0;
  for (const Slot::StackRec& r : sl.stacks) {
    if (r.tid == GetCurrentThreadId() || isRevive(r.tid)) continue;  // #28: a parked thread goes back below
    const ThreadKey* k = findKey(r.tid);
    for (const PThread& x : g_pt)
      if (x.tid == r.tid && k && x.created == k->created && x.suspended && x.mustIdle &&
          (x.esp != r.esp || x.eip != r.eip)) {
        if (nOff < 8) offs[nOff] = {x.tid, x.gameRet, x.esp, r.gameRet, r.esp, r.base};
        ++nOff;
      }
  }
  if (nOff) {
    Unpause();
    std::vector<Module> mods = Modules();
    std::string d;
    for (int i = 0; i < std::min(nOff, 8); ++i)
      d += Fmt("%s%lu %s now %s depth %u, at the save %s depth %u", i ? "; " : "", offs[i].tid,
               NameOf(offs[i].tid).c_str(), Where(mods, offs[i].eipNow).c_str(),
               (unsigned)(offs[i].base - offs[i].espNow), Where(mods, offs[i].eipSave).c_str(),
               (unsigned)(offs[i].base - offs[i].espSave));
    LOG("savestate: load slot %d: %d game thread(s) not at their wait of the save: %s", s + 1, nOff, d.c_str());
    g_retry = true;
    return Say(false, "Couldn't load right now - try again",
               Fmt("load: %d game thread(s) busy (%s) - not loaded, the game is unchanged", nOff,
                   NameOf(offs[0].tid).c_str()));
  }
  // 0b. (#28) a parked thread goes back only into a wait stub of the save, on its own committed stack; a thread to be
  // frozen or ended must wait idle (like a thread of the save of the same kind at the save, or at its usual rest).
  {
    const char* why = nullptr;
    DWORD whyTid = 0;
    for (int kr : revive) {
      const Slot::StackRec* r = nullptr;
      for (const Slot::StackRec& x : sl.stacks)
        if (x.tid == g_keep[kr].tid) r = &x;
      uintptr_t stub = r ? StubStart(r->eip) : 0;
      MEMORY_BASIC_INFORMATION mbi{};
      if (!r || !stub) why = "no wait of the save on record";
      else if (!VirtualQuery((void*)r->esp, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || !Writable(mbi.Protect) ||
               (uintptr_t)mbi.BaseAddress + mbi.RegionSize < r->base)
        why = "its stack is not there any more";
      if (why) {
        whyTid = g_keep[kr].tid;
        break;
      }
    }
    if (why) {
      Unpause();
      LOG("savestate: load slot %d: kept thread %lu %s cannot go back: %s", s + 1, whyTid, NameOf(whyTid).c_str(), why);
      DropSlot(s, "a game thread of the save cannot be brought back");
      return Say(false, Fmt("Slot %d was lost - save again", s + 1),
                 Fmt("load: slot %d is lost - game thread %s cannot be brought back (%s); save again", s + 1,
                     NameOf(whyTid).c_str(), why));
    }
    DWORD busy = 0;
    auto idle = [&](const PThread& t) {
      if (!t.suspended || !t.esp || !t.gameRet) return false;
      uint32_t d = (uint32_t)(t.stackBase - t.esp);
      for (const Slot::StackRec& x : sl.stacks)  // the same kind of thread waited here at the save
        if (x.start == t.start && x.gameRet == t.gameRet && x.base - x.esp == d) return true;
      uintptr_t rr;
      uint32_t rd;
      if (RestOf(t.tid, &rr, &rd) && (rd == d || RestOk(t.tid, d))) return true;
      // a thread made after the save (PhysX's new scene threads wait deeper than the old ones did): any wait on an
      // object called from game code (directly or through our wait hooks), not a lock wait
      return InWaitStub(t.eip) && !InLockWait(t.eip);
    };
    for (size_t i : freeze)
      if (!idle(g_pt[i])) busy = g_pt[i].tid;
    for (size_t i : kill)
      if (!idle(g_pt[i])) busy = g_pt[i].tid;
    if (busy) {
      Unpause();
      std::vector<Module> mods = Modules();
      std::string d;
      for (const PThread& t : g_pt)
        if (t.tid == busy) {
          d = Fmt("start %s wait %s depth %u eip %s;", Where(mods, t.start).c_str(), Where(mods, t.gameRet).c_str(),
                  (unsigned)(t.stackBase - t.esp), Where(mods, t.eip).c_str());
          for (const Slot::StackRec& x : sl.stacks)
            if (x.start == t.start) d += Fmt(" save-kind %lu %s/%u", x.tid, Where(mods, x.gameRet).c_str(), (unsigned)(x.base - x.esp));
        }
      LOG("savestate: load slot %d: thread %lu %s (made after the save) is not idle yet: %s", s + 1, busy,
          NameOf(busy).c_str(), d.c_str());
      g_retry = true;
      return Say(false, "Couldn't load right now - try again",
                 Fmt("load: a new game thread is busy (%s) - not loaded, the game is unchanged", NameOf(busy).c_str()));
    }
  }
  // 1. every allocation of the slot: still there (same base) or free to take back
  const char* fail = nullptr;
  uintptr_t failAt = 0;
  int reAlloc = 0;
  const bool hzHeld = HzTryLock(20000);  // #30: heap zombies (a frozen thread inside hkNtFreeVM holds it: refuse)
  if (!hzHeld) fail = "heap busy";
  for (const AllocRec& a : sl.allocs) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((void*)a.base, &mbi, sizeof mbi)) { fail = "query"; failAt = a.base; break; }
    if (mbi.State == MEM_FREE) {
      if (mbi.RegionSize < a.extent) { fail = "address taken"; failAt = a.base; break; }
      continue;
    }
    if ((uintptr_t)mbi.AllocationBase != a.base) { fail = "address taken"; failAt = a.base; break; }
    if (a.kind == 1) {  // still the game's (the OS may have given the address to someone else meanwhile)
      VaEntry* e = VaFindNoLock(a.base);
      if (!e || e->caller < exeBase || e->caller >= g_exeEnd) { fail = "address reused"; failAt = a.base; break; }
    } else if (!IsCrtSeg(a.base) && !IsOrphan(a.base) && !HzIsZombieNoLock(a.base)) {
      fail = "address reused (heap)";
      failAt = a.base;
      break;
    }
  }
  const bool refused = fail != nullptr;  // nothing written yet
  int hzBack = 0;
  if (!fail)  // #30: heap blocks of this slot the heap had released meanwhile (kept reserved) are its again
    for (const AllocRec& a : sl.allocs)
      if (a.kind == 2)
        for (int i = 0; i < g_hzZombieN; ++i)
          if (g_hzZombie[i] == a.base) {
            g_hzZombie[i] = g_hzZombie[--g_hzZombieN];
            ++hzBack;
            ++g_hzBack;
            break;
          }
  if (hzHeld) HzUnlock();
  if (hzBack) LOG("savestate: load slot %d: %d heap block(s) the heap had released are back (kept reserved)", s + 1, hzBack);
  if (!fail)  // allocations the game had released meanwhile (kept reserved) are live again
    for (const AllocRec& a : sl.allocs)
      if (VaEntry* e = a.kind == 1 ? VaFindNoLock(a.base) : nullptr; e && e->zombie) {
        e->zombie = 0;
        --g_zombies;
      }
  if (!fail)
    for (const AllocRec& a : sl.allocs) {
      MEMORY_BASIC_INFORMATION mbi;
      VirtualQuery((void*)a.base, &mbi, sizeof mbi);
      if (mbi.State != MEM_FREE) continue;
      if (!oVirtualAlloc((void*)a.base, a.extent, MEM_RESERVE, PAGE_READWRITE)) { fail = "re-reserve"; failAt = a.base; break; }
      if (a.caller) VaInsertNoLock(a.base, a.caller);
      ++reAlloc;
    }
  // 2. copy back (changed pages only), committing pages the game gave back meanwhile
  unsigned dirty = 0;
  int recommit = 0;
  View view;
  if (!fail)
    for (const RunRec& r : sl.runs) {
      MEMORY_BASIC_INFORMATION mbi;
      for (uintptr_t a = r.addr; a < r.addr + r.size;) {
        if (!VirtualQuery((void*)a, &mbi, sizeof mbi)) break;
        uintptr_t e = std::min<uintptr_t>((uintptr_t)mbi.BaseAddress + mbi.RegionSize, r.addr + r.size);
        if (mbi.State != MEM_COMMIT || !Writable(mbi.Protect) || (mbi.Protect & PAGE_GUARD)) {
          if (r.kind == 0 || !oVirtualAlloc((void*)a, e - a, MEM_COMMIT, PAGE_READWRITE)) { fail = "commit"; failAt = a; break; }
          ++recommit;
        }
        a = e;
      }
      if (fail) break;
      if (!packed && !CopySection(view, sl, r.off, (void*)r.addr, r.size, false, &dirty)) { fail = "map view"; break; }
    }
  ssstore::RestoreStats rst;
  if (!fail && packed) {  // #29: hash every page, decompress the changed ones (the tool's crew, in parallel)
    rst = ssstore::Restore(sl.pages, g_blocks.data(), g_blocks.size());
    dirty = rst.changed + rst.zeroed;
    if (!rst.ok) fail = "store read", failAt = 0;
  }
  long long t3 = RealQpc();
  // 3. exe allocations made after the save: nothing restored knows them
  int freed = 0;
  if (!fail && g_freeNew) {
    {
      for (size_t i = 0; i < kVaSlots && g_freeList.size() < g_freeList.capacity(); ++i) {
        uintptr_t b = g_va[i].base;
        if (b <= 1) continue;
        uintptr_t c = g_va[i].caller;
        if (c < exeBase || c >= g_exeEnd) continue;
        auto it = std::lower_bound(sl.allocs.begin(), sl.allocs.end(), b,
                                   [](const AllocRec& x, uintptr_t v) { return x.base < v; });
        if (it == sl.allocs.end() || it->base != b) g_freeList.push_back(b);
      }
    }
    for (uintptr_t b : g_freeList) {
      VaEntry* e = VaFindNoLock(b);
      if (e && e->zombie) continue;
      if (InAnySlotNoLock(b)) {  // the other slot needs it: keep it reserved
        if (e && oVirtualFree((void*)b, 0, MEM_DECOMMIT)) {
          e->zombie = 1;
          ++g_zombies;
        }
      } else if (oVirtualFree((void*)b, 0, MEM_RELEASE)) {
        VaRemoveNoLock(b);
        ++freed;
      }
    }
  }
  // 4. D3D objects: references the restored memory holds
  int raised = 0, lowered = 0;
  if (!fail) {
    // the game's references = those of the restored memory: the count at the save (0 for a resource made later or
    // already let go then). The references of the dropped timeline go; the render-thread scan then frees what is 0.
    for (Pin& p : g_pins) {
      int want = (p.mask & (1 << s)) ? p.slotRefs[s] : 0;
      int r = GameRefs(p.obj);
      for (; r < want; ++r) {
        p.obj->AddRef();
        ++raised;
      }
      for (; r > want; --r) {
        p.obj->Release();  // never the last reference: our pin stays
        ++lowered;
      }
    }
    g_pinRefsAdded += raised;
  }
  // 5. the game's clock back to the save
  int stkSet = 0, stkMiss = 0, reviveSet = 0;
  size_t stkBytes = 0;
  if (!fail)
    for (const Slot::StackRec& r : sl.stacks) {
      const uint8_t* src = sl.stackData.data() + r.off;
      if (r.tid == GetCurrentThreadId()) {
        if (r.esp != g_tickFrame || r.base != (uintptr_t)__readfsdword(4)) {
          ++stkMiss;
          continue;
        }
      } else if (isRevive(r.tid)) {  // #28: a parked thread: its wait of the save, called again (ss_ReviveJump)
        KeepRec* kp = nullptr;
        for (int kr : revive)
          if (g_keep[kr].tid == r.tid) kp = &g_keep[kr];
        uintptr_t stub = StubStart(r.eip), top = *(const uint32_t*)src;
        // a suspended wow64 thread in a syscall: eip = the stub's return from its call, esp = the stub's entry
        // ([esp] = the stub's caller); if [esp] is the return into the stub itself, the entry is 4 bytes up
        uintptr_t entry = (top >= stub && top < stub + 0x20) ? r.esp + 4 : r.esp;
        SsRevive& rv = kp->park.rev;
        rv.esp = (uint32_t)entry, rv.eip = (uint32_t)stub;
        rv.ebx = (uint32_t)r.ebx, rv.esi = (uint32_t)r.esi, rv.edi = (uint32_t)r.edi, rv.ebp = (uint32_t)r.ebp;
        rv.seh = (uint32_t)r.seh, rv.mxcsr = r.mxcsr, rv.fcw = r.fcw;
        rv.state = &kp->state;
        ++reviveSet;
      } else {
        const ThreadKey* k = findKey(r.tid);
        const PThread* t = nullptr;
        for (const PThread& x : g_pt)
          if (x.tid == r.tid && k && x.created == k->created && x.suspended && x.mustIdle) t = &x;
        if (!t || t->esp != r.esp || t->eip != r.eip) {
          ++stkMiss;
          continue;
        }
        CONTEXT c{};
        c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        if (GetThreadContext(t->h, &c)) {
          c.Ebx = r.ebx;
          c.Esi = r.esi;
          c.Edi = r.edi;
          c.Ebp = r.ebp;
          SetThreadContext(t->h, &c);
        }
      }
      for (size_t k = 0; k < r.size; k += 4)
        if (*(uint32_t*)(r.esp + k) != *(const uint32_t*)(src + k)) stkBytes += 4;
      memcpy((void*)r.esp, src, r.size);
      ++stkSet;
    }
  if (!fail && (rtRestart || rtNone)) {  // #27: the restored memory names the rendering thread of the save: it is gone
    *g_gIsThreadedRendering = 0;
    *g_gRenderingThread = 0;
    *g_gRenderingThreadRunnable = 0;
  }
  int tlsSet = 0;
  if (!fail)
    for (const Slot::TlsVal& v : sl.tls) {
      bool live = v.tid == GetCurrentThreadId();
      for (const PThread& t : g_pt) live |= t.tid == v.tid && t.teb == v.teb && t.suspended;
      if (!live) continue;
      if (uintptr_t* c = TlsCell(v.teb, v.index); c && *c != v.value) {
        *c = v.value;
        ++tlsSet;
      }
    }
  int flsBack = 0, flsCleared = 0, flsGone = 0;  // #27: FLS words into the snapshot as at the save
  if (!fail && g_flsOk > 0 && (g_inc & kIncFls)) {
    auto flsPut = [&](DWORD tid, uint64_t created, uintptr_t teb) {
      const Slot::FlsThread* was = nullptr;
      for (const Slot::FlsThread& x : sl.flsThreads)
        if (x.tid == tid && x.created == created) was = &x;
      ForFls(teb, [&](uint16_t pos, uintptr_t* cell, uintptr_t now) {
        uintptr_t old = 0;
        if (was)
          for (uint32_t i = was->begin; i < was->end; ++i)
            if (sl.fls[i].pos == pos) old = sl.fls[i].value;
        if (now == old || (!InSnapshotMem(now) && !InSnapshotMem(old))) return;
        *cell = old;
        ++(was ? flsBack : flsCleared);
      });
    };
    flsPut(GetCurrentThreadId(), g_gameCreated, (uintptr_t)NtCurrentTeb());
    for (const PtdThread& t : g_ptdThreads) flsPut(t.tid, t.created, t.teb);
    for (const Slot::FlsThread& x : sl.flsThreads) {  // ended since the save: their blocks are allocated again (leak)
      bool live = x.tid == GetCurrentThreadId();
      for (const PtdThread& t : g_ptdThreads) live |= x.tid == t.tid && x.created == t.created;
      flsGone += !live && x.end > x.begin;
    }
  }
  int filesLive = 0, filesClosed = 0, filesMoved = 0;
  if (!fail) filesLive = FilesLoadNoLock(s, &filesClosed, &filesMoved);
  int hLive = 0, hStates = 0, nFrozen = 0, nEnded = 0;  // #28
  std::vector<uintptr_t> killStacks;
  killStacks.reserve(kill.size());
  if (!fail) {
    hLive = HandlesLoadNoLock(s, &hStates);
    for (size_t i : freeze) {  // a thread of the other slot's timeline: frozen in its idle wait
      const PThread& t = g_pt[i];
      HANDLE own = OpenThread(THREAD_ALL_ACCESS, FALSE, t.tid);
      if (!own || SuspendThread(own) == (DWORD)-1) {
        if (own) CloseHandle(own);
        continue;
      }
      for (KeepRec& k : g_keep)
        if (k.state == kKeepFree) {
          k.tid = t.tid, k.created = t.created, k.start = t.start, k.h = own, k.byTerminate = false;
          k.exitCode = STILL_ACTIVE, k.nhv = 0, k.parkedAt = GetTickCount();
          k.state = kKeepFrozen;
          ++g_keepLive;
          ++g_freezes;
          ++nFrozen;
          own = nullptr;
          break;
        }
      if (own) {  // no room: end it instead (the other slot then cannot load)
        oTerminateThread(own, 0);
        CloseHandle(own);
        ++nEnded;
      }
    }
    for (size_t i : kill) {  // made after the save, no slot knows it: the restored game has no trace of it
      uintptr_t dealloc = 0;
      util::Read(g_pt[i].teb + 0xE0C, dealloc);  // TEB.DeallocationStack: its stack (WOW64 may keep it, see below)
      killStacks.push_back(dealloc);
      oTerminateThread(g_pt[i].h, 0);
      ++nEnded;
      ++g_kills;
    }
    for (int kr : unfreeze) g_keep[kr].state = kKeepReviving;  // resumed below
    for (int kr : revive) g_keep[kr].state = kKeepReviving;    // the thread frees its record (ss_ReviveJump)
    RebuildDeadNoLock();
  }
  int evSet = 0;
  if (!fail) {
    g_qpcOff = RealQpc() - sl.qpcVirtual;
    HANDLE now[4] = {};
    int n = GameEvents(now);
    for (int i = 0; i < n; ++i)
      for (int k = 0; k < sl.nEv; ++k)
        if (now[i] == sl.evH[k] && sl.evS[k] >= 0) {
          sl.evS[k] ? SetEvent(now[i]) : ResetEvent(now[i]);
          ++evSet;
        }
  }
  // (#26) heap segments the restored heaps no longer know (made after this save) are orphans: kept while another
  // slot needs them (nobody else can take an address still allocated), else given back.
  int orphKept = 0, orphFreed = 0;
  if (!fail) {
    g_orphanTmp.clear();
    auto consider = [&](uintptr_t b) {
      auto it = std::lower_bound(sl.allocs.begin(), sl.allocs.end(), b,
                                 [](const AllocRec& x, uintptr_t v) { return x.base < v; });
      if (it != sl.allocs.end() && it->base == b) return;  // the restored heap's own
      MEMORY_BASIC_INFORMATION mbi;
      if (!VirtualQuery((void*)b, &mbi, sizeof mbi) || mbi.State == MEM_FREE || (uintptr_t)mbi.AllocationBase != b) return;
      if (std::find(g_orphanTmp.begin(), g_orphanTmp.end(), b) != g_orphanTmp.end()) return;
      if (InAnySlotNoLock(b)) {
        if (g_orphanTmp.size() < g_orphanTmp.capacity()) g_orphanTmp.push_back(b), ++orphKept;
      } else if (oVirtualFree((void*)b, 0, MEM_RELEASE)) {
        ++orphFreed;
      }
    };
    for (uintptr_t b : g_crtSegs) consider(b);
    for (uintptr_t b : g_orphans) consider(b);
    std::sort(g_orphanTmp.begin(), g_orphanTmp.end());
    g_orphans.swap(g_orphanTmp);
  }
  if (!fail) {  // #28: kept threads of the save go on with the game (they run once the pause ends)
    for (int kr : unfreeze) {
      KeepRec& k = g_keep[kr];
      ResumeThread(k.h);
      CloseHandle(k.h);
      k.h = nullptr;
      k.state = kKeepFree;
      --g_keepLive;
      ++g_unfreezes;
    }
    for (int kr : revive) {
      KeepRec& k = g_keep[kr];
      CloseHandle(k.h);  // the thread's own copy in hkRtlExitUserThread is gone with that frame (a small leak)
      k.h = nullptr;
      k.park.cmd = 1;
      SetEvent(k.park.ev);
      --g_keepLive;
      ++g_revives;
    }
    RebuildDeadNoLock();
  }
  Unpause();
  long long t4 = RealQpc();
  storeLock.unlock();
  if (refused)
    return Say(false, Fmt("Slot %d can't load now - save again", s + 1),
               Fmt("load: slot %d cannot be loaded - its memory at %08X is used by something else now (%s); not "
                   "loaded, the game is unchanged",
                   s + 1, (unsigned)failAt, fail));
  if (fail)
    return Say(false, "Load failed - restart the game",
               Fmt("load: FAILED at %08X (%s) - the game may be damaged, restart it", (unsigned)failAt, fail));
  ++sl.loads;
  features::AfterStateLoad();  // #29: the tool's menu pause in the slot's memory is taken off (set again if open)
  {
    int vr = 0, vc = 0;
    VideosLoad(s, &vr, &vc);  // #29: in-world videos as at the save
    if (vr || vc) LOG("savestate: load slot %d: in-world videos: %d of the save live again, %d opened since closed", s + 1, vr, vc);
  }
  Say(true, Fmt("Loaded slot %d", s + 1), "");
  float pos[3];
  int hp;
  DantePos(game::W(), pos, &hp);
  game::Invalidate("save state loaded");  // #26: the tool's modules drop objects of the dropped timeline
  bool p2 = false;
  std::vector<uintptr_t> after = LoadedLevels(game::W(), &p2);  // the restored memory: the slot's parts
  LOG("savestate: load slot %d: level parts after the load (%zu, %s the save): %s", s + 1, after.size(),
      after == sl.levels ? "same as" : "NOT as", LevelNames(game::W()).c_str());
  {
    bool ok = true;
    std::string oc = OnexitCheck(&ok);
    int n = OnexitCount();
    LOG("savestate: load slot %d: msvcr100 %s; entries %d, at the save %d (%s); FLS words into the snapshot: %d put "
        "back, %d cleared (threads born after the save), %d threads ended since the save%s",
        s + 1, oc.c_str(), n, sl.onexitN, ok && n == sl.onexitN ? "as saved" : "NOT as saved", flsBack, flsCleared,
        flsGone, g_flsOk > 0 ? "" : (" - " + g_flsWhy).c_str());
  }
  // #28: the ended threads of the dropped timeline: wait until they are gone; a TerminateThread'ed WOW64 thread leaves
  // its 32-bit stack reserved (~1 MB of address space each, measured) - free it; the game's handles naming it are of
  // that timeline too (made after the save, no slot knows the thread) - close them.
  int stFreed = 0, hClosed = 0;
  unsigned long long stBytes = 0;
  for (size_t j = 0; j < kill.size(); ++j) {
    const PThread& t = g_pt[kill[j]];
    if (WaitForSingleObject(t.h, 2000) != WAIT_OBJECT_0) continue;
    if (uintptr_t b = killStacks[j]) {
      MEMORY_BASIC_INFORMATION mbi{};
      if (VirtualQuery((void*)b, &mbi, sizeof mbi) && mbi.State != MEM_FREE && (uintptr_t)mbi.AllocationBase == b &&
          mbi.Type == MEM_PRIVATE && !VaCaller(b)) {
        unsigned long long sz = 0;
        for (uintptr_t a = b; VirtualQuery((void*)a, &mbi, sizeof mbi) && (uintptr_t)mbi.AllocationBase == b &&
                              mbi.State != MEM_FREE;
             a += mbi.RegionSize)
          sz += mbi.RegionSize;
        if (oVirtualFree((void*)b, 0, MEM_RELEASE)) ++stFreed, stBytes += sz;
      }
    }
    HANDLE hv[8];
    int n = ThreadHandles(t.tid, hv, 8, nullptr);
    for (int k = 0; k < n; ++k)
      if (!OwnHandle(hv[k]) && ThreadCreated(hv[k]) == t.created) {  // that very thread (ids are reused)
        {
          Spin lk(&g_fileLock);
          if (HRec* r = HFindNoLock(hv[k])) HRemoveNoLock(r);
        }
        oCloseHandle(hv[k]);
        ++hClosed;
      }
  }
  if (!revive.empty() || !unfreeze.empty() || nFrozen || nEnded || hLive)
    LOG("savestate: load slot %d: threads back into their wait %d (stacks set %d), resumed %d, frozen %d, ended %d "
        "(stacks freed %d, %.1f MB; their handles closed %d); kernel objects made live again %d (state put back %d); "
        "kept threads now %d",
        s + 1, (int)revive.size(), reviveSet, (int)unfreeze.size(), nFrozen, nEnded, stFreed, stBytes / 1048576.0,
        hClosed, hLive, hStates, g_keepLive.load());
  return Fmt("load slot %d (#%d, %s): %u pages changed (%.1f MB), game stopped %.1f ms (copy %.1f%s, %d tries), total "
             "%.1f ms; re-reserved %d, re-committed %d, freed %d new allocations, D3D refs +%d -%d, files: %d reopened "
             "%d closed %d moved, events %d, stacks %d (%zu B changed, %d not at the same wait), TLS %d/%zu, kept %ld; "
             "threads: %d back, %d resumed, %d frozen, %d ended; objects %d; level parts %zu -> %zu (%s); heap zombies %d "
             "(kept %ld, back %ld, freed %ld; slot heap blocks %d; releases %ld, from ntdll %ld); %s",
             s + 1, sl.loads, packed ? "packed" : "raw", dirty, dirty * 4096 / 1048576.0, QpcMs(t2, t4), QpcMs(t2, t3),
             packed ? Fmt(": hash %.1f + restore %.1f on %d threads", rst.hashMs, rst.ms - rst.hashMs, rst.threads).c_str() : "",
             tries, QpcMs(t0, t4),
             reAlloc, recommit, freed, raised, lowered, filesLive, filesClosed, filesMoved, evSet, stkSet, stkBytes,
             stkMiss, tlsSet, sl.tls.size(), g_zombies.load(), (int)revive.size(), (int)unfreeze.size(), nFrozen,
             nEnded, hLive, levels.size(), after.size(), after == sl.levels ? "as saved" : "NOT as saved",
             g_hzZombieN, g_hzTaken.load(), g_hzBack.load(), g_hzFreed.load(), g_hzKnownN, g_hzCalls.load(),
             g_hzNtdll.load(), ProcMem().c_str());
}

// FEngineLoop::Tick, the game's main loop: `while (!GIsRequestingExit) GEngineLoop.Tick();`
constexpr const char* kSigMainLoop = "B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 39 1D ?? ?? ?? ?? 74 EE";
using Tick_t = void(__fastcall*)(void*, void*);
Tick_t oTick = nullptr;
bool g_keyF5 = false, g_keyF9 = false;

bool GameForeground() {
  DWORD pid = 0;
  GetWindowThreadProcessId(GetForegroundWindow(), &pid);
  return pid == GetCurrentProcessId();
}

void DropSlot(int i, const char* why) {
  Slot& sl = g_slots[i];
  if (!sl.valid) return;
  g_dropWhy[i] = why;
  int z = 0;
  {
    Spin pl(&g_pinLock);
    for (Pin& p : g_pins) p.mask &= ~(1 << i);
    Spin vl(&g_vaLock);
    sl.valid = false;
    z = ReleaseZombiesNoLock();
    z += PruneOrphansNoLock();
    Spin fl(&g_fileLock);
    FilesDropSlotNoLock(i);
    HandlesDropSlotNoLock(i);  // #28
  }
  UpdateQuarantine();
  RebuildKnown();  // #28
  int rel = ReleaseKept();
  FreeStore(sl);  // #29
  VideosDropSlot(i);
  SlotChanged(i, false);
  LOG("savestate: slot %d dropped (%s): %d kept allocations freed, %d kept threads released; %s", i + 1, why, z, rel,
      ProcMem().c_str());
  SetMsg(Fmt("slot %d emptied: %s", i + 1, why));
}

// Free address space of the (32-bit, large-address-aware) game: total and the largest block.
void FreeVa(unsigned long long* total, unsigned long long* biggest) {
  MEMORY_BASIC_INFORMATION mbi;
  *total = *biggest = 0;
  for (uintptr_t a = 0x10000; a < 0xFFFF0000 && VirtualQuery((void*)a, &mbi, sizeof mbi);) {
    if (mbi.State == MEM_FREE) {
      *total += mbi.RegionSize;
      *biggest = std::max<unsigned long long>(*biggest, mbi.RegionSize);
    }
    uintptr_t n = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    if (n <= a) break;
    a = n;
  }
}

// #26: slots stay across level streaming and map changes (a slot of another map is refused, not dropped). They keep
// memory alive, so: low address space -> the slot on another map, else the older one, goes. And a streaming log
// (parts loaded / unloaded) for the tests. Game thread.
std::atomic<bool> g_streamLog{true};
unsigned long long g_valveTotalMB = 450, g_valveBiggestMB = 160;
void WatchSlots() {
  static DWORD lastS = 0, lastV = 0, lastM = 0, lastMem = 0;
  DWORD now = GetTickCount();
  if (now - lastM >= 3000) {  // #28: the game's waits go through our imports from the start (a thread that began its
    lastM = now;              // wait before the patch waits 16 bytes higher after it: not "at the wait of the save")
    static DWORD lastCount = 0;
    HMODULE mods[1024];
    DWORD need = 0;
    if (EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need) && need != lastCount) {
      lastCount = need;
      RefreshModules();
      PatchGameModules();
    }
  }
  const game::World& w = game::W();
  if (g_streamLog && now - lastS >= 200 && w.mission) {
    lastS = now;
    static std::string prev, prevMap;
    std::string cur = LevelNames(w);
    if (w.map != prevMap) prev.clear();
    if (cur != prev) {
      std::vector<std::string> a, b;
      auto split = [](const std::string& s, std::vector<std::string>& v) {
        size_t p = 0;
        while (p < s.size()) {
          size_t e = s.find(' ', p);
          if (e == std::string::npos) e = s.size();
          v.push_back(s.substr(p, e - p));
          p = e + 1;
        }
      };
      split(prev, a);
      split(cur, b);
      std::string d;
      for (auto& x : b)
        if (std::find(a.begin(), a.end(), x) == a.end()) d += " +" + x;
      for (auto& x : a)
        if (std::find(b.begin(), b.end(), x) == b.end()) d += " -" + x;
      if (!prev.empty() || !d.empty()) LOG("savestate: streaming%s", d.c_str());
      prev = cur;
      prevMap = w.map;
    }
  }
  if (g_storeGc.exchange(false)) {  // #29: slots emptied elsewhere (device reset, ss arm 0): their copies go
    for (int i = 0; i < kSlots; ++i)
      if (!g_slots[i].valid && (g_slots[i].packed || !g_slots[i].pages.empty() || g_slots[i].sec))
        FreeStore(g_slots[i]), VideosDropSlot(i);
    for (int i = 0; i < kSlots; ++i) SlotChanged(i, g_slots[i].valid);
  }
  if (now - lastMem >= 1000) {  // #29: memory numbers for the Save states tab
    lastMem = now;
    UpdateMemView(w);
  }
  if (now - lastV < 2000 || !AnySlotValid()) return;
  lastV = now;
  // (#28: a slot stays when its map is opened again - its threads and kernel objects are kept, see "kept threads")
  unsigned long long tot, big;
  FreeVa(&tot, &big);
  if (tot >= (g_valveTotalMB << 20) && big >= (g_valveBiggestMB << 20)) return;
  // #29: the oldest slot of another map, else of another opening of this map (they keep memory the game does not
  // use), else the oldest. The player is told (a slot never goes silently).
  int victim = -1;
  auto older = [](int a, int b) {
    FILETIME x, y;
    SystemTimeToFileTime(&g_slots[a].when, &x);
    SystemTimeToFileTime(&g_slots[b].when, &y);
    return CompareFileTime(&x, &y) < 0;
  };
  for (int pass = 0; pass < 3 && victim < 0; ++pass)
    for (int i = 0; i < kSlots; ++i) {
      const Slot& x = g_slots[i];
      if (!x.valid) continue;
      bool pick = pass == 0 ? _stricmp(x.map.c_str(), w.map.c_str()) != 0 : pass == 1 ? x.world != w.world : true;
      if (pick && (victim < 0 || older(i, victim))) victim = i;
    }
  LOG("savestate: low address space (free %.0f MB, largest %.0f MB)", tot / 1048576.0, big / 1048576.0);
  DropSlot(victim, "the game was low on memory");
  Toast(false, Fmt("Slot %d emptied: the game was low on memory", victim + 1));
}

void __fastcall hkTick(void* self, void* edx) {
  g_tickFrame = (uintptr_t)_AddressOfReturnAddress() + 4;  // the main loop's frame and above
  {
    static uintptr_t lastWorld = 0;
    if (uintptr_t wd = game::W().world; wd && wd != lastWorld) lastWorld = wd, ++g_mapEpoch;
  }
  if (g_armed) WatchSlots();  // (#29: the keys are binds of the Save states tab: features ss_save_N / ss_load_N)
  if (int op = g_reqOp.load()) {
    // #26: while a part of the level streams in/out, wait (up to 5 s), then refuse
    std::string busyWhat;
    DWORD waited = GetTickCount() - g_reqAt;
    // (#26: StreamingBusy flags proved useless - volume streaming does not set them, and one part stays "to unload"
    // for minutes, blocking every save. The wait relies on the game's threads being at rest + open videos.)
    bool busy = false;
    if (busy && waited < 5000) {
      static DWORD lastMsg = 0;
      if (GetTickCount() - lastMsg > 500) {
        lastMsg = GetTickCount();
        SetMsg(Fmt("%s: waiting for the level to finish streaming (%s)", op == 1 ? "save" : "load", busyWhat.c_str()));
      }
      oTick(self, edx);
      return;
    }
    int s = std::clamp(g_reqSlot.load(), 0, kSlots - 1);
    g_retry = false;
    g_say.clear();
    std::string m;
    if (!g_armed) {
      m = Say(false, "Save states are off - turn them on in the Save states tab", "save states are off");
    } else if (op == 1) {
      m = DoSave(s);
    } else if (op == 3 || op == 4) {  // #29: empty slot s / all slots (asked by the player: no "emptied by itself")
      int n = 0;
      for (int i = op == 4 ? 0 : s; i < (op == 4 ? kSlots : s + 1); ++i)
        if (g_slots[i].valid) DropSlot(i, "emptied"), g_dropWhy[i].clear(), SlotChanged(i, false), ++n;
      m = op == 4 ? Fmt("%d slots emptied - their memory is free again", n)
                  : Fmt("slot %d emptied - its memory is free again", s + 1);
      Say(true, op == 4 ? std::string(n ? "All slots emptied" : "All slots are empty") : Fmt("Slot %d emptied", s + 1), "");
    } else {
      m = DoLoad(s);
    }
    if (g_retry && waited < 5000) {  // game threads busy: the request stays, next frame
      static DWORD lastMsg = 0;
      if (GetTickCount() - lastMsg > 500) lastMsg = GetTickCount(), SetMsg(m);
      oTick(self, edx);
      return;
    }
    g_reqOp = 0;
    if (waited > 30 && !busy) m += Fmt(" (waited %lu ms)", waited);
    LOG("savestate: %s", m.c_str());
    if (g_armed) LOG("savestate: state after %s: %s", op == 1 ? "save" : "load", StateLine().c_str());
    if (g_autoShots) {  // the next frame, from another thread (Screenshot waits for a Present)
      std::wstring path = util::ExeDir() + L"DmCPT_dumps\\ss_" + std::to_wstring(++g_shotN) +
                          (op == 1 ? L"_save" : L"_load") + std::to_wstring(s + 1) + L".bmp";
      HANDLE t = CreateThread(nullptr, 0, [](LPVOID p) -> DWORD {
        std::wstring* ws = (std::wstring*)p;
        bool ok = d3d9hook::Screenshot(*ws);
        LOG("savestate: screenshot %s %s", util::WideToUtf8(*ws).c_str(), ok ? "ok" : "FAILED");
        delete ws;
        return 0;
      }, new std::wstring(path), 0, nullptr);
      if (t) CloseHandle(t);
    }
    SetMsg(m);
    if (!g_say.empty()) Toast(g_sayOk, g_say);
    else if (op == 1 || op == 2)  // a refusal without its own text
      Toast(false, op == 1 ? "Couldn't save - try again" : "Couldn't load - try again");
    ++g_done;
  }
  oTick(self, edx);
}

bool g_tickHooked = false;

// Research: while save states are on, a crash of the game (heap corruption, access violation outside the tool)
// writes a minidump to DmCPT_dumps\savestate_crash_<n>.dmp (the game's own handler misses heap corruption).
using MiniDumpWriteDump_t = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                          PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
MiniDumpWriteDump_t g_mdw = nullptr;
wchar_t g_dumpPath[MAX_PATH] = {};
volatile LONG g_dumped = 0;
uintptr_t g_selfBase = 0, g_selfEnd = 0;

LONG CALLBACK CrashCatcher(EXCEPTION_POINTERS* e) {
  DWORD code = e->ExceptionRecord->ExceptionCode;
  if (!g_armed || !g_mdw || (code != 0xC0000374 && code != 0xC0000005 && code != 0xC0000409)) return EXCEPTION_CONTINUE_SEARCH;
  uintptr_t at = (uintptr_t)e->ExceptionRecord->ExceptionAddress;
  if (code == 0xC0000005 && at >= g_selfBase && at < g_selfEnd) return EXCEPTION_CONTINUE_SEARCH;  // SafeRead
  if (InterlockedCompareExchange(&g_dumped, 1, 0) != 0) return EXCEPTION_CONTINUE_SEARCH;
  {  // where each register points: region, owner (our VirtualAlloc table), heap of a slot (no heap use here)
    wchar_t tp[MAX_PATH];
    wcscpy_s(tp, g_dumpPath);
    if (wchar_t* dot = wcsrchr(tp, L'.')) wcscpy_s(dot, 5, L".txt");
    HANDLE t = CreateFileW(tp, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (t != INVALID_HANDLE_VALUE) {
      const CONTEXT* c = e->ContextRecord;
      const struct {
        const char* n;
        uintptr_t v;
      } regs[] = {{"eax", c->Eax}, {"ebx", c->Ebx}, {"ecx", c->Ecx}, {"edx", c->Edx}, {"esi", c->Esi},
                  {"edi", c->Edi}, {"ebp", c->Ebp}, {"esp", c->Esp}, {"eip", c->Eip}};
      char line[256];
      for (auto& r : regs) {
        MEMORY_BASIC_INFORMATION mbi{};
        VirtualQuery((void*)r.v, &mbi, sizeof mbi);
        uintptr_t ab = (uintptr_t)mbi.AllocationBase;
        VaEntry* ve = VaFindNoLock(ab);
        int n = snprintf(line, sizeof line,
                         "%s=%08X base %08X state %X type %X prot %X | va caller %08X (exe+%X) zombie %u | slot heap %d\r\n",
                         r.n, (unsigned)r.v, (unsigned)ab, (unsigned)mbi.State, (unsigned)mbi.Type,
                         (unsigned)mbi.Protect, ve ? (unsigned)ve->caller : 0,
                         ve && ve->caller > g_exeBase ? (unsigned)(ve->caller - g_exeBase) : 0, ve ? ve->zombie : 0,
                         (int)IsCrtSeg(ab));
        DWORD w;
        WriteFile(t, line, n, &w, nullptr);
      }
      CloseHandle(t);
    }
  }
  HANDLE f = CreateFileW(g_dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  if (f != INVALID_HANDLE_VALUE) {
    MINIDUMP_EXCEPTION_INFORMATION mei{GetCurrentThreadId(), e, FALSE};
    g_mdw(GetCurrentProcess(), GetCurrentProcessId(), f,
          (MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo), &mei, nullptr, nullptr);
    CloseHandle(f);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

// #28: ntdll!RtlExitUserThread (kept threads park there). Only with save states (config / Dev switch).
void InstallExitHook() {
  if (oRtlExitUserThread) return;
  void* ex = (void*)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlExitUserThread");
  if (ex && MH_CreateHook(ex, (void*)&hkRtlExitUserThread, (void**)&oRtlExitUserThread) == MH_OK) MH_EnableHook(ex);
}

void InstallEarly() {
  AddVectoredExceptionHandler(1, NameCatcher);
  g_exeBase = (uintptr_t)GetModuleHandleW(nullptr);
  {
    auto nt = (IMAGE_NT_HEADERS*)(g_exeBase + ((IMAGE_DOS_HEADER*)g_exeBase)->e_lfanew);
    g_exeEnd = g_exeBase + nt->OptionalHeader.SizeOfImage;
  }
  g_exeLo = g_exeBase;
  g_exeHi = g_exeEnd;
  g_fixedVa = GetFileAttributesW(util::InExeDir(L"DmCPT_fixedva.txt").c_str()) != INVALID_FILE_ATTRIBUTES;
  HMODULE kb = GetModuleHandleW(L"KernelBase.dll");
  if (!kb) return;
  MH_STATUS s = MH_Initialize();
  if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) return;
  void* va = (void*)GetProcAddress(kb, "VirtualAlloc");
  void* vf = (void*)GetProcAddress(kb, "VirtualFree");
  if (va && MH_CreateHook(va, (void*)&hkVirtualAlloc, (void**)&oVirtualAlloc) == MH_OK) MH_EnableHook(va);
  if (vf && MH_CreateHook(vf, (void*)&hkVirtualFree, (void**)&oVirtualFree) == MH_OK) MH_EnableHook(vf);
  // #30: the heaps' own releases (ntdll -> NtFreeVirtualMemory), see hkNtFreeVM
  if (HMODULE nt = GetModuleHandleW(L"ntdll.dll")) {
    auto dos = (IMAGE_DOS_HEADER*)nt;
    auto nth = (IMAGE_NT_HEADERS*)((uint8_t*)nt + dos->e_lfanew);
    g_ntdllLo = (uintptr_t)nt;
    g_ntdllHi = g_ntdllLo + nth->OptionalHeader.SizeOfImage;
    void* nf = (void*)GetProcAddress(nt, "NtFreeVirtualMemory");
    if (nf && MH_CreateHook(nf, (void*)&hkNtFreeVM, (void**)&oNtFreeVM) == MH_OK && MH_EnableHook(nf) == MH_OK)
      LOG("savestate: NtFreeVirtualMemory hooked (heap releases kept for slots)");
    else
      oNtFreeVM = (NtFreeVM_t)nf;
  }
  oQPC = (QPC_t)PatchExeImport("KERNEL32.dll", "QueryPerformanceCounter", (void*)&hkQPC);
  // #26: the game's files (tracked only while save states are on)
  oCreateFileW = (CreateFileW_t)PatchExeImport("KERNEL32.dll", "CreateFileW", (void*)&hkCreateFileW);
  oCloseHandle = (CloseHandle_t)PatchExeImport("KERNEL32.dll", "CloseHandle", (void*)&hkCloseHandle);
  oBinkOpen = (BinkOpen_t)PatchExeImport("binkw32.dll", "_BinkOpen@8", (void*)&hkBinkOpen);
  oBinkClose = (BinkClose_t)PatchExeImport("binkw32.dll", "_BinkClose@4", (void*)&hkBinkClose);
  if (!oBinkOpen || !oBinkClose) {  // both or none (a count from one side only would be wrong)
    if (oBinkOpen) PatchExeImport("binkw32.dll", "_BinkOpen@8", (void*)oBinkOpen);
    if (oBinkClose) PatchExeImport("binkw32.dll", "_BinkClose@4", (void*)oBinkClose);
    oBinkOpen = nullptr;
    oBinkClose = nullptr;
  }
  if (!oCloseHandle) oCloseHandle = &CloseHandle;
  if (void* ta = (void*)GetProcAddress(kb, "TlsAlloc"))
    if (MH_CreateHook(ta, (void*)&hkTlsAlloc, (void**)&oTlsAlloc) == MH_OK) MH_EnableHook(ta);
  {  // #28: kept threads - the real functions behind the game's patched imports, and thread exit
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    oWFSO = (WFSO_t)GetProcAddress(k32, "WaitForSingleObject");
    oWFSOEx = (WFSOEx_t)GetProcAddress(k32, "WaitForSingleObjectEx");
    oWFMO = (WFMO_t)GetProcAddress(k32, "WaitForMultipleObjects");
    oWFMOEx = (WFMOEx_t)GetProcAddress(k32, "WaitForMultipleObjectsEx");
    oGECT = (GECT_t)GetProcAddress(k32, "GetExitCodeThread");
    oTerminateThread = (TT_t)GetProcAddress(k32, "TerminateThread");
    oDeleteCs = (DelCs_t)GetProcAddress(k32, "DeleteCriticalSection");
    for (HANDLE& e : g_signalled) e = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    if (WantEarly()) {  // else when save states are turned on (SetArmed)
      InstallExitHook();
      // the exe's waits through our imports from its first instruction: a thread that started waiting before the
      // patch waits 16 bytes higher once it waits again (UE3's pool threads sleep for minutes) - a slot saved in
      // between could not load (#28, Load checkpoint series)
      PatchModuleImports(g_exeBase);
    }
  }
  // research: heap blocks by caller
  if (GetFileAttributesW(util::InExeDir(L"DmCPT_heaptrack.txt").c_str()) != INVALID_FILE_ATTRIBUTES) {
    g_ht = (HeapRec*)oVirtualAlloc(nullptr, kHtSlots * sizeof(HeapRec), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    void* a = (void*)GetProcAddress(ntdll, "RtlAllocateHeap");
    void* f = (void*)GetProcAddress(ntdll, "RtlFreeHeap");
    void* r = (void*)GetProcAddress(ntdll, "RtlReAllocateHeap");
    if (g_ht && a && f && r && MH_CreateHook(a, (void*)&hkRtlAlloc, (void**)&oRtlAlloc) == MH_OK &&
        MH_CreateHook(f, (void*)&hkRtlFree, (void**)&oRtlFree) == MH_OK &&
        MH_CreateHook(r, (void*)&hkRtlReAlloc, (void**)&oRtlReAlloc) == MH_OK) {
      MH_EnableHook(a);
      MH_EnableHook(f);
      MH_EnableHook(r);
    }
  }
}

void Init() {
  ssstore::Start();  // #29: page store (packer + load crew threads, idle until a save)
  if (!oQPC) LOG("savestate: QueryPerformanceCounter import not found - loads would jump the game clock");
  uintptr_t site = ue3::SigScan(kSigMainLoop);
  if (!site) {
    LOG("savestate: main loop signature not found - save states unavailable");
    return;
  }
  uintptr_t tick = site + 10 + *(int32_t*)(site + 6);
  g_ntQueryEvent = (NtQueryEvent_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryEvent");
  if (uintptr_t fes = ue3::SigScan(kSigFrameEndSync)) g_frameEndSync = *(uint32_t*)(fes + 2);
  LOG("savestate: FFrameEndSync at %08X", (unsigned)g_frameEndSync);
  FindRenderThreadFns();
  FindObjectTypes();  // #28
  LOG("savestate: RtlExitUserThread hook %s", oRtlExitUserThread ? "on" : "FAILED - slots cannot survive a map reload");
  LOG("savestate: FEngineLoop::Tick at RVA 0x%X (main loop RVA 0x%X); heap tracking %s",
      (unsigned)(tick - g_exeBase), (unsigned)(site - g_exeBase), g_ht ? "ON" : "off");
  if (hooks::Create((void*)tick, (void*)&hkTick, (void**)&oTick, "FEngineLoop::Tick") &&
      hooks::Enable((void*)tick, "FEngineLoop::Tick"))
    g_tickHooked = true;
}

bool WantEarly() {
  static int v = -1;
  if (v >= 0) return v == 1;
  v = 1;  // #29 (user): on by default - only an explicit "savestates": false turns them off
  HANDLE f = CreateFileW(util::InExeDir(L"DmCPracticeTool.json").c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                         OPEN_EXISTING, 0, nullptr);
  if (f == INVALID_HANDLE_VALUE) return true;
  static char buf[1 << 16];
  DWORD n = 0;
  ReadFile(f, buf, sizeof buf - 1, &n, nullptr);
  CloseHandle(f);
  buf[n] = 0;
  const char* k = strstr(buf, "\"savestates\"");  // #29: Save states tab "Enable save states"
  if (!k) k = strstr(buf, "\"dev_savestates\"");  // #25-28 prototype: the same switch
  if (k) {
    const char* c = strchr(k, ':');
    while (c && (*++c == ' ' || *c == '\t')) {
    }
    v = c && strncmp(c, "true", 4) == 0 ? 1 : 0;
  }
  return v == 1;
}

void InstallCrashCatcher() {
  if (g_mdw) return;
  if (HMODULE dh = LoadLibraryW(L"dbghelp.dll")) g_mdw = (MiniDumpWriteDump_t)GetProcAddress(dh, "MiniDumpWriteDump");
  swprintf(g_dumpPath, MAX_PATH, L"%sDmCPT_dumps\\savestate_crash_%u.dmp", util::ExeDir().c_str(),
           (unsigned)GetTickCount());
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     (LPCWSTR)&CrashCatcher, &self);
  MODULEINFO mi{};
  GetModuleInformation(GetCurrentProcess(), self, &mi, sizeof mi);
  g_selfBase = (uintptr_t)mi.lpBaseOfDll;
  g_selfEnd = g_selfBase + mi.SizeOfImage;
  AddVectoredExceptionHandler(1, CrashCatcher);
}

void OnGameDevice(void* device) {
  if (g_devHooked || !device || !WantEarly()) return;
  if (InstallDeviceHooks(*reinterpret_cast<void***>(device))) {
    g_early = true;
    InstallCrashCatcher();  // #26: on from the start never went through SetArmed - no dump of the M02 crash
    g_armed = true;
    StartSampler();
    LOG("savestate: on from the game's device %p (config savestates)", device);
  }
}

bool Available() { return g_tickHooked; }
bool Armed() { return g_armed; }

void SetArmed(bool on) {
  if (on == g_armed && !(on && !g_mdw)) return;
  if (on) {
    if (!g_tickHooked) {
      SetMsg("unavailable: main loop not found");
      return;
    }
    if (!InstallDeviceHooks()) {
      SetMsg("unavailable: D3D9 hooks failed");
      return;
    }
    InstallCrashCatcher();
    InstallExitHook();  // #28
    g_armed = true;
    StartSampler();
    SetMsg(g_early ? "on" : "on - turned on late: D3D9 resources created before are not protected (dev_savestates "
                            "in the config = on from the game start)");
    LOG("savestate: armed");
  } else {
    g_armed = false;
    g_samplerOn = false;
    Spin lk(&g_pinLock);
    for (Pin& p : g_pins) p.mask = 0;  // released by the render thread scan
    {
      Spin vl(&g_vaLock);
      for (Slot& s : g_slots) s.valid = false;
      ReleaseZombiesNoLock();
      PruneOrphansNoLock();
      Spin fl(&g_fileLock);
      for (int i = 0; i < kSlots; ++i) FilesDropSlotNoLock(i), HandlesDropSlotNoLock(i);
    }
    UpdateQuarantine();
    RebuildKnown();
    ReleaseKept();
    g_storeGc = true;  // #29
    SetMsg("off");
    LOG("savestate: disarmed");
  }
}

void OnRenderFrame() {
  if (g_pins.empty()) return;
  if (InterlockedCompareExchange(&g_pinLock, 1, 0) != 0) return;  // busy: next frame
  size_t n = g_pins.size();
  for (size_t k = 0; k < 2048 && n; ++k) {
    if (g_pinScan >= n) g_pinScan = 0;
    Pin& p = g_pins[g_pinScan];
    if (!p.mask && GameRefs(p.obj) <= 0) {
      p.obj->Release();
      ++g_pinDropped;
      p = g_pins.back();
      g_pins.pop_back();
      --n;
    } else {
      ++g_pinScan;
    }
  }
  InterlockedExchange(&g_pinLock, 0);
}

void OnDeviceReset() {
  // Default-pool resources must all be gone before Reset, and slots point at the old GPU resources: every slot goes,
  // the pins of what Reset destroys go. #29: the rest (managed textures and buffers, shaders...) the game keeps through
  // the Reset - their pins stay, else a slot saved after it points at them unprotected (load -> crash in d3d9 on the
  // rendering thread, Fullscreen + Alt+Tab, 17.11 21:56).
  bool had = AnySlotValid();
  Spin lk(&g_pinLock);
  {
    Spin vl(&g_vaLock);
    for (Slot& s : g_slots) s.valid = false;
    ReleaseZombiesNoLock();
    PruneOrphansNoLock();
    Spin fl(&g_fileLock);
    for (int i = 0; i < kSlots; ++i) FilesDropSlotNoLock(i), HandlesDropSlotNoLock(i);
  }
  UpdateQuarantine();
  RebuildKnown();
  ReleaseKept();
  size_t kept = 0;
  for (size_t i = 0; i < g_pins.size();) {
    Pin& p = g_pins[i];
    p.mask = 0;
    if (p.keep) {
      ++i, ++kept;
      continue;
    }
    p.obj->Release();
    ++g_pinDropped;
    p = g_pins.back();
    g_pins.pop_back();
  }
  LOG("savestate: device reset: %zu D3D resources stay pinned (they live through the Reset)", kept);
  g_storeGc = true;  // #29: their copies go on the game thread
  if (g_armed) SetMsg("device reset: slots cleared");
  if (had) {
    LOG("savestate: device reset (Alt+Tab in Fullscreen?) - every slot emptied");
    Toast(false, "Save states emptied: the game reset its graphics (Alt+Tab in Fullscreen)");
  }
}

void Request(int op, int slot) {
  g_reqSlot = slot;
  g_reqForce = false;
  g_reqAt = GetTickCount();
  g_reqOp = op;
}

int ActiveSlot() { return g_activeSlot; }
void SetActiveSlot(int s) { g_activeSlot = std::clamp(s, 0, kSlots - 1); }

std::string Message() {
  std::lock_guard<std::mutex> lk(g_msgMu);
  return g_msg;
}

SlotView SlotInfo(int s) {
  std::lock_guard<std::mutex> lk(g_viewMu);
  return g_views[std::clamp(s, 0, kSlots - 1)];
}

MemView Memory() {
  std::lock_guard<std::mutex> lk(g_viewMu);
  return g_mem;
}

bool Thumb(int s, unsigned* gen, std::vector<uint8_t>* bgra, int* w, int* h) {
  std::lock_guard<std::mutex> lk(g_viewMu);
  const ThumbRec& t = g_thumbs[std::clamp(s, 0, kSlots - 1)];
  if (t.gen == *gen) return false;
  *gen = t.gen;
  *bgra = t.bgra;
  *w = t.w;
  *h = t.h;
  return true;
}

// #29: the slot's picture: the next presented frame after its save (render thread, before the tool draws on it).
// 16:9 middle of the back buffer, scaled down twice (linear) to kThumbW x kThumbH, kept as BGRA in the tool.
void OnPresent(void* device) {
  int s = g_thumbWant.load();
  if (s < 0 || !device) return;
  g_thumbWant = -1;
  auto* dev = (IDirect3DDevice9*)device;
  IDirect3DSurface9 *bb = nullptr, *mid = nullptr, *sml = nullptr, *sys = nullptr;
  bool ok = false;
  constexpr int kW = 320, kH = 180;
  if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
    D3DSURFACE_DESC d{};
    bb->GetDesc(&d);
    RECT src{0, 0, (LONG)d.Width, (LONG)d.Height};
    if (d.Width * 9 > d.Height * 16) {  // wider than 16:9: the middle
      LONG w = (LONG)(d.Height * 16 / 9);
      src.left = (LONG)(d.Width - w) / 2, src.right = src.left + w;
    } else if (d.Width * 9 < d.Height * 16) {
      LONG h = (LONG)(d.Width * 9 / 16);
      src.top = (LONG)(d.Height - h) / 2, src.bottom = src.top + h;
    }
    if ((d.Format == D3DFMT_X8R8G8B8 || d.Format == D3DFMT_A8R8G8B8) &&
        SUCCEEDED(dev->CreateRenderTarget(kW * 2, kH * 2, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &mid, nullptr)) &&
        SUCCEEDED(dev->CreateRenderTarget(kW, kH, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &sml, nullptr)) &&
        SUCCEEDED(dev->StretchRect(bb, &src, mid, nullptr, D3DTEXF_LINEAR)) &&
        SUCCEEDED(dev->StretchRect(mid, nullptr, sml, nullptr, D3DTEXF_LINEAR)) &&
        SUCCEEDED(dev->CreateOffscreenPlainSurface(kW, kH, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
        SUCCEEDED(dev->GetRenderTargetData(sml, sys))) {
      D3DLOCKED_RECT lr;
      if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        std::vector<uint8_t> px((size_t)kW * kH * 4);
        for (int y = 0; y < kH; ++y) {
          const uint8_t* r = (const uint8_t*)lr.pBits + y * lr.Pitch;
          uint8_t* o = px.data() + (size_t)y * kW * 4;
          memcpy(o, r, (size_t)kW * 4);
          for (int x = 0; x < kW; ++x) o[x * 4 + 3] = 255;
        }
        sys->UnlockRect();
        std::lock_guard<std::mutex> lk(g_viewMu);
        ThumbRec& t = g_thumbs[s];
        t.bgra.swap(px);
        t.w = kW;
        t.h = kH;
        t.gen = ++g_thumbGen;
        g_views[s].thumbGen = t.gen;
        ok = true;
      }
    }
  }
  for (IDirect3DSurface9* x : {sys, sml, mid, bb})
    if (x) x->Release();
  if (!ok) LOG("savestate: slot %d picture FAILED", s + 1);
}

std::string SlotText(int s) {
  s = std::clamp(s, 0, kSlots - 1);
  const Slot& sl = g_slots[s];
  if (!sl.valid) return g_dropWhy[s].empty() ? "empty" : "empty: " + g_dropWhy[s];
  return Fmt("%02d:%02d:%02d  %s  %.0f MB %s  stop %.0f ms  loads %d", sl.when.wHour, sl.when.wMinute, sl.when.wSecond,
             sl.map.c_str(), sl.used / 1048576.0, sl.packed ? "packed" : "raw", sl.pauseMs, sl.loads);
}

std::vector<std::string> HeapStats() {
  if (!g_ht) return {"heap tracking off (Binaries\\Win32\\DmCPT_heaptrack.txt at start turns it on)"};
  std::vector<Module> mods = Modules();
  uintptr_t crt = 0;
  Heaps(&crt);
  uintptr_t proc = (uintptr_t)GetProcessHeap();
  struct Agg {
    unsigned long long bytes = 0;
    long n = 0;
  };
  std::map<std::string, Agg> agg;
  std::map<std::string, Agg> exeSites;
  {
    std::vector<HeapRec> copy;
    copy.reserve(1 << 20);
    {
      Spin lk(&g_htLock);
      for (size_t i = 0; i < kHtSlots; ++i)
        if (g_ht[i].key) copy.push_back(g_ht[i]);
    }
    for (const HeapRec& r : copy) {
      std::string h = r.heap == proc ? "process" : r.heap == crt ? "msvcr100" : Fmt("%08X", (unsigned)r.heap);
      const Module* m1 = ModuleAt(mods, r.c1);
      std::string who = m1 ? m1->name : "?";
      if (r.c2 && (!m1 || m1->base != g_exeBase)) who += " <- exe";
      Agg& a = agg[h + " | " + who];
      a.bytes += r.size;
      ++a.n;
      uintptr_t ex = (r.c1 >= g_exeBase && r.c1 < g_exeEnd) ? r.c1 : r.c2;
      if (ex) {
        Agg& e = exeSites[h + " | exe+" + Fmt("%X", (unsigned)(ex - g_exeBase))];
        e.bytes += r.size;
        ++e.n;
      }
    }
  }
  std::vector<std::string> out{Fmt("heap blocks live %ld (dropped %ld)", g_htLive.load(), g_htDrop.load())};
  auto dump = [&](std::map<std::string, Agg>& m, size_t top) {
    std::vector<std::pair<unsigned long long, std::string>> v;
    for (auto& [k, a] : m) v.push_back({a.bytes, Fmt("%10.1f MB %8ld blocks  %s", a.bytes / 1048576.0, a.n, k.c_str())});
    std::sort(v.rbegin(), v.rend());
    for (size_t i = 0; i < v.size() && i < top; ++i) out.push_back(v[i].second);
  };
  out.push_back("by heap | caller module:");
  dump(agg, 40);
  out.push_back("by heap | exe call site:");
  dump(exeSites, 25);
  return out;
}

std::vector<std::string> StackScan(unsigned long tid, int maxLines) {
  std::vector<std::string> out;
  std::vector<Module> mods = Modules();
  uintptr_t esp = 0;
  ThreadInfo t = Inspect(tid, tid != GetCurrentThreadId());
  if (tid == GetCurrentThreadId()) {
    volatile int here = 0;
    esp = (uintptr_t)&here;
  } else if (t.ctxOk) {
    esp = t.ctx.Esp;
  }
  if (!esp || !t.stackBase) return {"stack scan: no context"};
  // copy the stack first (the thread runs on)
  size_t n = std::min<size_t>((t.stackBase - esp) / 4, 1 << 18);
  std::vector<uintptr_t> st(n);
  util::SafeRead(esp, st.data(), n * 4);
  char b[256];
  snprintf(b, sizeof b, "stack scan tid %lu (%s): esp %08X base %08X, %zu dwords; eip %s", tid,
           NameOf(tid).c_str(), (unsigned)esp, (unsigned)t.stackBase, n,
           t.ctxOk ? Where(mods, t.ctx.Eip).c_str() : "-");
  out.push_back(b);
  int lines = 0;
  for (size_t i = 0; i < n && lines < maxLines; ++i) {
    uintptr_t v = st[i];
    const Module* m = ModuleAt(mods, v);
    if (!m) continue;
    uintptr_t target = 0;
    const char* kind = "";
    if (!CallBefore(v, &target, &kind)) continue;
    snprintf(b, sizeof b, "  [+%05X from base -%05X] ret %s  %s %s", (unsigned)(i * 4),
             (unsigned)(t.stackBase - (esp + i * 4)), Where(mods, v).c_str(), kind,
             target ? Where(mods, target).c_str() : "");
    out.push_back(b);
    ++lines;
  }
  return out;
}

std::vector<std::string> Probe(bool stacks) {
  std::vector<std::string> out;
  char b[512];
  std::vector<Module> mods = Modules();
  static std::vector<Export> ntEx = Exports(GetModuleHandleW(L"ntdll.dll"));
  static std::vector<Export> w32Ex =
      GetModuleHandleW(L"win32u.dll") ? Exports(GetModuleHandleW(L"win32u.dll")) : std::vector<Export>{};
  HMODULE self = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     (LPCWSTR)&Probe, &self);

  // ---- threads
  std::vector<DWORD> tids = ThreadIds();
  std::map<uintptr_t, std::string> stackOf;  // allocation base of a stack -> thread
  snprintf(b, sizeof b, "threads: %zu (names caught: %d)", tids.size(), g_nameCount.load());
  out.push_back(b);
  for (DWORD tid : tids) {
    ThreadInfo t = Inspect(tid, true);
    std::string wait = "-";
    if (t.ctxOk) {
      const Module* m = ModuleAt(mods, t.ctx.Eip);
      if (m && _stricmp(m->name.c_str(), "ntdll.dll") == 0)
        wait = ExportAt(ntEx, t.ctx.Eip);
      else if (m && _stricmp(m->name.c_str(), "win32u.dll") == 0)
        wait = ExportAt(w32Ex, t.ctx.Eip);
      else
        wait = "RUNNING " + Where(mods, t.ctx.Eip);
    }
    // first return address on the stack that is outside the system dlls: who waits
    std::string who;
    if (t.ctxOk && t.stackBase > t.ctx.Esp) {
      size_t n = std::min<size_t>((t.stackBase - t.ctx.Esp) / 4, 4096);
      std::vector<uintptr_t> st(n);
      util::SafeRead(t.ctx.Esp, st.data(), n * 4);
      for (uintptr_t v : st) {
        const Module* m = ModuleAt(mods, v);
        if (!m) continue;
        const char* nm = m->name.c_str();
        if (!_stricmp(nm, "ntdll.dll") || !_stricmp(nm, "KERNELBASE.dll") || !_stricmp(nm, "KERNEL32.DLL") ||
            !_stricmp(nm, "win32u.dll") || !_stricmp(nm, "user32.dll") || !_stricmp(nm, "msvcr100.dll"))
          continue;
        uintptr_t tg;
        const char* k;
        if (!CallBefore(v, &tg, &k)) continue;
        who = Where(mods, v);
        break;
      }
    }
    if (t.stackAlloc) stackOf[t.stackAlloc] = std::to_string(tid);
    snprintf(b, sizeof b, "  tid %5lu %-22s start %-34s %-28s from %s  stack %s%s", tid, NameOf(tid).c_str(),
             Where(mods, t.start).c_str(), wait.c_str(), who.c_str(),
             MB(t.stackBase - t.stackLimit).c_str(), tid == GetCurrentThreadId() ? "  (this)" : "");
    out.push_back(b);
    if (stacks) {
      std::vector<std::string> s = StackScan(tid, 24);
      out.insert(out.end(), s.begin(), s.end());
    }
  }

  // ---- memory map
  uintptr_t crtHeap = 0;
  std::vector<uintptr_t> heaps = Heaps(&crtHeap);
  uintptr_t procHeap = (uintptr_t)GetProcessHeap();
  struct Cat {
    unsigned long long commit = 0, writable = 0;
    int regions = 0;
  };
  std::map<std::string, Cat> cats;
  std::map<uintptr_t, unsigned long long> heapSeg;  // heap region allocation bases -> heap handle
  for (uintptr_t h : heaps) heapSeg[h] = h;
  // segments/large blocks of every heap (HeapWalk takes the heap lock; fine outside a pause)
  for (uintptr_t h : heaps) {
    PROCESS_HEAP_ENTRY e{};
    uintptr_t lastBase = 0, lastEnd = 0;
    int guard = 0;
    if (!HeapLock((HANDLE)h)) continue;
    while (HeapWalk((HANDLE)h, &e) && ++guard < 4000000) {
      uintptr_t a = (uintptr_t)e.lpData;
      if (a >= lastBase && a < lastEnd) continue;
      MEMORY_BASIC_INFORMATION mbi;
      if (!VirtualQuery((void*)a, &mbi, sizeof mbi)) continue;
      lastBase = (uintptr_t)mbi.AllocationBase;
      lastEnd = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
      heapSeg[lastBase] = h;
    }
    HeapUnlock((HANDLE)h);
  }
  std::map<std::string, unsigned long long> otherByCaller;
  std::vector<std::pair<unsigned long long, uintptr_t>> unknownBig;
  std::map<uintptr_t, unsigned long long> unknownSize;
  uintptr_t a = 0x10000;
  MEMORY_BASIC_INFORMATION mbi;
  unsigned long long total = 0, totalW = 0, reserved = 0;
  while (a < 0xFFFF0000 && VirtualQuery((void*)a, &mbi, sizeof mbi)) {
    uintptr_t next = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    if (next <= a) break;
    if (mbi.State == MEM_RESERVE) reserved += mbi.RegionSize;
    if (mbi.State == MEM_COMMIT) {
      uintptr_t ab = (uintptr_t)mbi.AllocationBase;
      bool w = Writable(mbi.Protect) && !(mbi.Protect & PAGE_GUARD);
      std::string cat;
      if (mbi.Type == MEM_IMAGE) {
        const Module* m = ModuleAt(mods, ab);
        cat = "image " + std::string(m ? m->name : "?");
        if ((HMODULE)ab == self) cat = "image <tool dll>";
      } else if (mbi.Type == MEM_MAPPED) {
        cat = "mapped";
      } else if (stackOf.count(ab)) {
        cat = "private stack";
      } else if (heapSeg.count(ab)) {
        uintptr_t h = heapSeg[ab];
        cat = h == procHeap ? "private heap process" : h == crtHeap ? "private heap msvcr100" : "private heap other";
        if (h != procHeap && h != crtHeap) {
          snprintf(b, sizeof b, "private heap %08X", (unsigned)h);
          cat = b;
        }
      } else if (uintptr_t c = VaCaller(ab)) {
        const Module* m = ModuleAt(mods, c);
        cat = "private VA " + std::string(m ? ((HMODULE)m->base == self ? "<tool dll>" : m->name) : "?");
        if (m && w) otherByCaller[Where(mods, c)] += mbi.RegionSize;
      } else {
        cat = "private unknown";
        if (w) unknownSize[ab] += mbi.RegionSize;
      }
      Cat& c = cats[cat];
      c.commit += mbi.RegionSize;
      if (w) c.writable += mbi.RegionSize;
      ++c.regions;
      total += mbi.RegionSize;
      if (w) totalW += mbi.RegionSize;
    }
    a = next;
  }
  PROCESS_MEMORY_COUNTERS_EX pmc{sizeof pmc};
  GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof pmc);
  snprintf(b, sizeof b, "memory: committed %s (writable %s), reserved-only %s; private bytes %s, working set %s; "
           "VA table live %ld / total %ld; heaps %zu (process %08X, msvcr100 %08X)",
           MB(total).c_str(), MB(totalW).c_str(), MB(reserved).c_str(), MB(pmc.PrivateUsage).c_str(),
           MB(pmc.WorkingSetSize).c_str(), g_vaLive.load(), g_vaTotal.load(), heaps.size(), (unsigned)procHeap,
           (unsigned)crtHeap);
  out.push_back(b);
  std::vector<std::pair<std::string, Cat>> sorted(cats.begin(), cats.end());
  std::sort(sorted.begin(), sorted.end(), [](auto& x, auto& y) { return x.second.writable > y.second.writable; });
  for (auto& [k, c] : sorted) {
    snprintf(b, sizeof b, "  %-44s writable %10s  committed %10s  regions %d", k.c_str(), MB(c.writable).c_str(),
             MB(c.commit).c_str(), c.regions);
    out.push_back(b);
  }
  std::vector<std::pair<unsigned long long, std::string>> callers;
  for (auto& [k, v] : otherByCaller) callers.push_back({v, k});
  std::sort(callers.rbegin(), callers.rend());
  out.push_back("VirtualAlloc callers (writable committed):");
  for (size_t i = 0; i < callers.size() && i < 30; ++i) {
    snprintf(b, sizeof b, "  %10s  %s", MB(callers[i].first).c_str(), callers[i].second.c_str());
    out.push_back(b);
  }
  for (auto& [k, v] : unknownSize) unknownBig.push_back({v, k});
  std::sort(unknownBig.rbegin(), unknownBig.rend());
  out.push_back("unknown private (largest):");
  for (size_t i = 0; i < unknownBig.size() && i < 15; ++i) {
    snprintf(b, sizeof b, "  %10s  at %08X", MB(unknownBig[i].first).c_str(), (unsigned)unknownBig[i].second);
    out.push_back(b);
  }
  return out;
}

void DevUi() {
  ImGui::SeparatorText("SAVESTATES (debug - the controls are in the Save states tab; commands: ss ...)");
  if (!g_tickHooked) {
    ImGui::TextDisabled("unavailable: the game's main loop was not found");
    return;
  }
  ImGui::Text("on %d (from the game start %d), config savestates %d", (int)g_armed.load(), (int)g_early,
              (int)g_cfg.saveStates);
  for (int i = 0; i < kSlots; ++i) ImGui::TextUnformatted(("slot " + std::to_string(i + 1) + ": " + SlotText(i)).c_str());
  ssstore::Stats st = ssstore::GetStats();
  ImGui::TextWrapped("store %.1f MB in %u chunks (live %.1f MB), %u entries; %s", st.chunkBytes / 1048576.0, st.chunks,
                     st.liveBytes / 1048576.0, st.entries, st.last.c_str());
  ImGui::TextWrapped("%s", Message().c_str());
}

// #26 research: the process layout to compare two runs (slots after a restart need the same addresses).
std::vector<std::string> Layout(const std::string& tag) {
  std::vector<std::string> L;
  std::vector<Module> mods = Modules();
  L.push_back(Fmt("fixedva %d arena hits %ld miss %ld next %08X", (int)g_fixedVa, g_arenaHits.load(), g_arenaMiss.load(),
                  (unsigned)g_arenaNext));
  for (const Module& m : mods) {
    auto nt = (IMAGE_NT_HEADERS*)(m.base + ((IMAGE_DOS_HEADER*)m.base)->e_lfanew);
    WORD dc = nt->OptionalHeader.DllCharacteristics;
    L.push_back(Fmt("module %08X %08X dynbase %d preferred %08X %s", (unsigned)m.base, (unsigned)(m.end - m.base),
                    (dc & IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE) ? 1 : 0, (unsigned)nt->OptionalHeader.ImageBase,
                    m.name.c_str()));
  }
  uintptr_t crt = 0;
  for (uintptr_t h : Heaps(&crt)) L.push_back(Fmt("heap %08X%s", (unsigned)h, h == crt ? " msvcr100" : ""));
  RefreshModules();
  for (DWORD tid : ThreadIds()) {
    ThreadInfo t = Inspect(tid, false);
    L.push_back(Fmt("thread %-24s start %-30s stack %08X-%08X", NameOf(tid).c_str(), Where(mods, t.start).c_str(),
                    (unsigned)t.stackAlloc, (unsigned)t.stackBase));
  }
  int n = std::min(g_valogN.load(), kVaLogMax);
  for (int i = 0; i < n; ++i) {
    const VaLogRec& r = g_valog[i];
    L.push_back(Fmt("va %5d %08X size %08X type %X caller exe+%X t %lu", i, (unsigned)r.addr, (unsigned)r.size,
                    (unsigned)r.type, (unsigned)(r.caller - g_exeBase), (unsigned long)(r.tick - g_valog[0].tick)));
  }
  std::wstring path = util::ExeDir() + L"DmCPT_dumps\\ss_layout_" + util::Utf8ToWide(tag) + L".txt";
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
  if (f != INVALID_HANDLE_VALUE) {
    for (auto& s : L) {
      DWORD w;
      WriteFile(f, s.data(), (DWORD)s.size(), &w, nullptr);
      WriteFile(f, "\r\n", 2, &w, nullptr);
    }
    CloseHandle(f);
  }
  return {Fmt("layout: %zu modules, %d exe VirtualAllocs logged (of %d) -> %s", mods.size(), n, g_valogN.load(),
              util::WideToUtf8(path).c_str())};
}

// #26 research: a slot on disk - pages deduplicated (zero / same content), unique pages compressed (XPRESS, 64 KB
// chunks), written, read back and checked page by page. Sizes and times for stage 2.
std::vector<std::string> DiskTest(int s, bool keep) {
  Slot& sl = g_slots[s];
  if (!sl.valid) return {Fmt("disk: slot %d is empty", s + 1)};
  using Comp_t = LONG(NTAPI*)(USHORT, PUCHAR, ULONG, PUCHAR, ULONG, ULONG, PULONG, PVOID);
  using Decomp_t = LONG(NTAPI*)(USHORT, PUCHAR, ULONG, PUCHAR, ULONG, PULONG, PVOID);
  using Ws_t = LONG(NTAPI*)(USHORT, PULONG, PULONG);
  HMODULE nt = GetModuleHandleW(L"ntdll.dll");
  auto comp = (Comp_t)GetProcAddress(nt, "RtlCompressBuffer");
  auto decomp = (Decomp_t)GetProcAddress(nt, "RtlDecompressBufferEx");
  auto wsz = (Ws_t)GetProcAddress(nt, "RtlGetCompressionWorkSpaceSize");
  constexpr USHORT kFmt = 3 | 0x0000;  // COMPRESSION_FORMAT_XPRESS, standard engine
  ULONG ws1 = 0, ws2 = 0;
  if (!comp || !decomp || !wsz || wsz(kFmt, &ws1, &ws2) < 0) return {"disk: no RtlCompressBuffer"};
  std::vector<uint8_t> work(std::max(ws1, ws2) + 16);
  const uint64_t pages = sl.used / 4096;
  auto hash = [](const uint8_t* p) {
    uint64_t h = 0x9E3779B97F4A7C15ull;
    const uint64_t* q = (const uint64_t*)p;
    for (int i = 0; i < 512; ++i) h = (h ^ q[i]) * 0x100000001B3ull + (h >> 29);
    return h;
  };
  std::vector<uint32_t> table(pages);  // 0 = zero page, else 1 + index of the unique page
  std::unordered_map<uint64_t, uint32_t> seen;
  seen.reserve((size_t)pages);
  uint32_t unique = 0;
  uint64_t zero = 0, dup = 0;
  std::wstring path = util::ExeDir() + L"DmCPT_dumps\\ss_slot" + std::to_wstring(s + 1) + L".bin";
  HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (f == INVALID_HANDLE_VALUE) return {"disk: cannot create the file"};
  long long t0 = RealQpc();
  std::vector<uint8_t> chunk(65536), out(65536 + 8192);
  size_t inChunk = 0;
  uint64_t written = 0, compBytes = 0;
  double compMs = 0;
  auto flush = [&]() {
    if (!inChunk) return;
    ULONG cs = 0;
    long long c0 = RealQpc();
    LONG st = comp(kFmt, chunk.data(), (ULONG)inChunk, out.data() + 8, (ULONG)out.size() - 8, 4096, &cs, work.data());
    compMs += QpcMs(c0, RealQpc());
    if (st < 0 || cs >= inChunk) {  // incompressible: stored
      cs = (ULONG)inChunk;
      memcpy(out.data() + 8, chunk.data(), inChunk);
      *(uint32_t*)out.data() = 0x80000000u | cs;
    } else {
      *(uint32_t*)out.data() = cs;
    }
    *(uint32_t*)(out.data() + 4) = (uint32_t)inChunk;
    DWORD w;
    WriteFile(f, out.data(), cs + 8, &w, nullptr);
    written += cs + 8;
    compBytes += cs;
    inChunk = 0;
  };
  {
    DWORD w;
    uint64_t hdr[2] = {0x31544C5354504D44ull, pages};  // "DMPTSLT1"
    WriteFile(f, hdr, sizeof hdr, &w, nullptr);
    written += sizeof hdr;
    View v;
    std::vector<uint8_t> pg(4096);
    unsigned d = 0;
    for (uint64_t i = 0; i < pages; ++i) {
      // read the page out of the section (toSection=false copies section -> pg)
      if (!CopySection(v, sl, i * 4096, pg.data(), 4096, false, &d)) break;
      const uint64_t* q = (const uint64_t*)pg.data();
      bool z = true;
      for (int k = 0; k < 512 && z; ++k) z = q[k] == 0;
      if (z) {
        table[i] = 0;
        ++zero;
        continue;
      }
      uint64_t h = hash(pg.data());
      auto it = seen.find(h);
      if (it != seen.end()) {
        table[i] = it->second + 1;
        ++dup;
        continue;
      }
      seen.emplace(h, unique);
      table[i] = ++unique;
      memcpy(chunk.data() + inChunk, pg.data(), 4096);
      inChunk += 4096;
      if (inChunk == chunk.size()) flush();
    }
    flush();
    WriteFile(f, table.data(), (DWORD)(table.size() * 4), &w, nullptr);
    written += table.size() * 4;
    FlushFileBuffers(f);
    CloseHandle(f);
  }
  double writeMs = QpcMs(t0, RealQpc());
  // read back: decompress every chunk, rebuild every page and compare with the slot
  long long r0 = RealQpc();
  uint64_t bad = 0;
  double readMs = 0;
  {
    HANDLE g = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (g == INVALID_HANDLE_VALUE) return {"disk: cannot read the file back"};
    LARGE_INTEGER fs;
    GetFileSizeEx(g, &fs);
    std::vector<uint8_t> file((size_t)fs.QuadPart);
    DWORD rd = 0;
    for (size_t at = 0; at < file.size();) {
      DWORD want = (DWORD)std::min<size_t>(file.size() - at, 64u << 20);
      if (!ReadFile(g, file.data() + at, want, &rd, nullptr) || !rd) break;
      at += rd;
    }
    CloseHandle(g);
    std::vector<uint8_t> uniq((size_t)unique * 4096);
    size_t at = 16, u = 0;
    while (u < uniq.size() && at + 8 <= file.size()) {
      uint32_t cs = *(uint32_t*)&file[at], raw = *(uint32_t*)&file[at + 4];
      bool stored = (cs & 0x80000000u) != 0;
      cs &= 0x7FFFFFFF;
      if (stored) memcpy(&uniq[u], &file[at + 8], raw);
      else {
        ULONG fin = 0;
        decomp(kFmt, &uniq[u], raw, &file[at + 8], cs, &fin, work.data());
      }
      u += raw;
      at += 8 + cs;
    }
    readMs = QpcMs(r0, RealQpc());
    const uint32_t* tb = (const uint32_t*)&file[at];
    View v;
    std::vector<uint8_t> pg(4096);
    static const uint8_t zeros[4096] = {};
    unsigned d = 0;
    for (uint64_t i = 0; i < pages; ++i) {
      if (!CopySection(v, sl, i * 4096, pg.data(), 4096, false, &d)) break;
      const uint8_t* want = tb[i] ? &uniq[(size_t)(tb[i] - 1) * 4096] : zeros;
      if (memcmp(pg.data(), want, 4096) != 0) ++bad;
    }
  }
  // the other slot: pages at the same address with the same content (a delta between two slots)
  std::string cross = "other slot empty";
  int oi = -1;
  for (int i = 0; i < kSlots && oi < 0; ++i)
    if (i != s && g_slots[i].valid) oi = i;
  if (oi >= 0) {
    const Slot& o = g_slots[oi];
    std::map<uintptr_t, uint64_t> at;  // address of a page -> offset in the other slot
    for (const RunRec& r : o.runs)
      for (uint32_t k = 0; k < r.size; k += 4096) at[r.addr + k] = r.off + k;
    uint64_t same = 0, both = 0;
    View v1, v2;
    std::vector<uint8_t> p1(4096), p2(4096);
    unsigned d = 0;
    for (const RunRec& r : sl.runs)
      for (uint32_t k = 0; k < r.size; k += 4096) {
        auto it = at.find(r.addr + k);
        if (it == at.end()) continue;
        ++both;
        if (CopySection(v1, sl, r.off + k, p1.data(), 4096, false, &d) &&
            CopySection(v2, o, it->second, p2.data(), 4096, false, &d) && memcmp(p1.data(), p2.data(), 4096) == 0)
          ++same;
      }
    cross = Fmt("vs slot %d: %llu pages at the same address, %llu identical (%.0f%%) -> a delta would be %.1f MB",
                oi + 1, both, same, both ? same * 100.0 / both : 0.0, (pages - same) * 4096 / 1048576.0);
  }
  if (!keep) DeleteFileW(path.c_str());
  return {Fmt("disk slot %d: %.1f MB = %llu pages: zero %llu, duplicate %llu, unique %u (%.1f MB) -> compressed %.1f MB, "
              "file %.1f MB (%.0f%% of the slot)",
              s + 1, sl.used / 1048576.0, pages, zero, dup, unique, unique * 4096 / 1048576.0, compBytes / 1048576.0,
              written / 1048576.0, written * 100.0 / sl.used),
          Fmt("disk slot %d: write %.0f ms (of it compress %.0f ms), read + decompress %.0f ms, check: %llu bad pages%s",
              s + 1, writeMs, compMs, readMs, bad, keep ? "; file kept" : "; file deleted"),
          cross};
}

std::vector<std::string> Command(const std::vector<std::string>& a) {
  std::string c = a.empty() ? "status" : a[0];
  auto arg = [&](size_t i, int def) { return a.size() > i ? atoi(a[i].c_str()) : def; };
  if (c == "arm") {
    SetArmed(arg(1, 1) != 0);
    return {Fmt("armed %d: %s", (int)g_armed.load(), Message().c_str())};
  }
  if (c == "save" || c == "load") {  // ss save|load [slot] [force]  (force: no wait for streaming - tests)
    int before = g_done;
    Request(c == "save" ? 1 : 2, arg(1, g_activeSlot + 1) - 1);
    if (a.size() > 2 && a[2] == "force") g_reqForce = true;
    for (int i = 0; i < 300 && g_done == before; ++i) Sleep(50);
    return {g_done == before ? std::string("timed out (is the game running a frame?)") : Message()};
  }
  if (c == "loop") {  // loop <count> <slot> <ms between>: loads in a row
    std::vector<std::string> out;
    int n = arg(1, 20), slot = arg(2, g_activeSlot + 1) - 1, ms = arg(3, 500);
    for (int i = 0; i < n; ++i) {
      int before = g_done;
      Request(2, slot);
      for (int k = 0; k < 300 && g_done == before; ++k) Sleep(50);
      std::string m = g_done == before ? "timed out" : Message();
      LOG("savestate: loop %d/%d: %s", i + 1, n, m.c_str());
      if (m.find("FAILED") != std::string::npos || m.find("timed out") != std::string::npos) {
        out.push_back(m);
        break;
      }
      Sleep(ms);
    }
    out.push_back("loop done: " + ProcMem());
    return out;
  }
  if (c == "opt") {
    if (a.size() > 1) g_inc = (unsigned)strtoul(a[1].c_str(), nullptr, 0);
    if (a.size() > 3 && a[2] == "free") g_freeNew = a[3] != "0";
    return {Fmt("include mask %u (1 exe data, 2 exe VirtualAlloc, 4 msvcr100 heap, 8 physics dlls, 16 msvcr100/msvcp100 .data, 32 FLS slots), free new %d",
                g_inc.load(), (int)g_freeNew.load())};
  }
  if (c == "heap") return HeapStats();
  if (c == "shots") {
    g_autoShots = arg(1, 1) != 0;
    return {Fmt("screenshot after each save/load: %d", (int)g_autoShots.load())};
  }
  if (c == "mem") return {ProcMem()};
  if (c == "empty") {  // #27: ss empty <slot>
    int before = g_done;
    Request(3, arg(1, g_activeSlot + 1) - 1);
    for (int i = 0; i < 100 && g_done == before; ++i) Sleep(50);
    return {Message(), ProcMem()};
  }
  if (c == "threads") {  // #27 research: ss threads [tag]: every thread (name, start, creation) + handle count, so
                         // two lists (before / after a death, restart...) can be compared
    std::vector<std::string> out;
    std::vector<Module> mods = Modules();
    DWORD hc = 0;
    GetProcessHandleCount(GetCurrentProcess(), &hc);
    std::vector<DWORD> ids = ThreadIds();
    out.push_back(Fmt("threads %s: %zu threads, %lu handles, render thread %lu", a.size() > 1 ? a[1].c_str() : "",
                      ids.size(), hc, RenderTid()));
    for (DWORD tid : ids) {
      ThreadInfo ti = Inspect(tid, false);
      out.push_back(Fmt("  T %lu %-22s start %s created %llu", tid, NameOf(tid).c_str(), Where(mods, ti.start).c_str(),
                        ti.created));
    }
    return out;
  }
  if (c == "keep") {  // #28: kept threads, held kernel objects, counters
    std::vector<std::string> out;
    DWORD hc = 0;
    GetProcessHandleCount(GetCurrentProcess(), &hc);
    out.push_back(Fmt("kept threads %d (parks %ld, back %ld, frozen %ld, resumed %ld, ended %ld, game TerminateThread "
                      "held %ld, released %ld, waits answered %ld, sliced %ld, late thread handles %ld, ended by id %ld); known threads %d (handles %d); objects tracked %ld, held %ld "
                      "(closes held back %ld, made live %ld, states put back %ld); handles %lu; critical section "
                      "deletes held back %ld; exit hook %s",
                      g_keepLive.load(), g_parks.load(), g_revives.load(), g_freezes.load(), g_unfreezes.load(),
                      g_kills.load(), g_termHeld.load(), g_released.load(), g_waitsFaked.load(), g_slicedWaits.load(), g_lateWatch.load(), g_deadByTid.load(),
                      g_knownN, g_watchN.load(),
                      g_hrecLive.load(), g_hZombies.load(), g_hHeld.load(), g_hRevived.load(), g_hStateSet.load(), hc,
                      g_csHeld.load(), oRtlExitUserThread ? "on" : "off"));
    for (const KeepRec& k : g_keep)
      if (k.state != kKeepFree)
        out.push_back(Fmt("  K %lu %-20s state %ld%s exit %lu handles %d since %lu ms", k.tid, NameOf(k.tid).c_str(),
                          k.state, k.byTerminate ? " (game TerminateThread)" : "", k.exitCode, k.nhv,
                          GetTickCount() - k.parkedAt));
    return out;
  }
  if (c == "fls") {  // #27 research: TEB.FlsData of this thread and a few others, chunk words that point at memory
    std::vector<std::string> out;
    auto dump = [&](DWORD tid, uintptr_t teb) {
      uintptr_t f = util::ReadOr<uintptr_t>(teb + 0xFB4);
      std::string l = Fmt("thread %lu teb %08X (self %08X, +0x2000 self %08X FlsData %08X) FlsData %08X:", tid,
                          (unsigned)teb, (unsigned)util::ReadOr<uintptr_t>(teb + 0x18),
                          (unsigned)util::ReadOr<uintptr_t>(teb + 0x2000 + 0x18),
                          (unsigned)util::ReadOr<uintptr_t>(teb + 0x2000 + 0xFB4), (unsigned)f);
      for (int i = 0; f && i < 16; ++i) l += Fmt(" %08X", (unsigned)util::ReadOr<uintptr_t>(f + i * 4));
      out.push_back(l);
      for (int i = 0; f && i < 16; ++i) {
        uintptr_t c = util::ReadOr<uintptr_t>(f + i * 4);
        if (c < 0x10000 || c == f) continue;
        SIZE_T sz = HeapValidate(GetProcessHeap(), 0, (void*)c) ? HeapSize(GetProcessHeap(), 0, (void*)c) : 0;
        std::string w = Fmt("  [+%X] -> %08X (process heap block %ld B):", i * 4, (unsigned)c, (long)sz);
        for (SIZE_T k = 0; k < sz && k < 0x200; k += 4) w += Fmt(" %X", (unsigned)util::ReadOr<uintptr_t>(c + k));
        out.push_back(w);
      }
    };
    std::vector<std::string> l2 = engine::OnGame<std::vector<std::string>>([&] {
      out.clear();
      HMODULE crt = GetModuleHandleW(L"msvcr100.dll");
      auto gp = crt ? (void*(__cdecl*)())GetProcAddress(crt, "_getptd") : nullptr;
      out.push_back(Fmt("game thread msvcr100 _ptd %08X", gp ? (unsigned)(uintptr_t)gp() : 0u));
      dump(GetCurrentThreadId(), (uintptr_t)NtCurrentTeb());
      ListGameThreads();
      int n = 0;
      for (const PtdThread& t : g_ptdThreads)
        if (n++ < 3) dump(t.tid, t.teb);
      CheckFlsLayout();
      out.push_back(g_flsWhy);
      return out;
    });
    return l2;
  }
  if (c == "crt") {  // #27: msvcr100's atexit table + HeapValidate of the snapshot heaps; crt add <n>: n atexit()
                     // calls (a no-op) like the game's function-local statics - the table grows / moves (tests)
    std::vector<std::string> out;
    if (a.size() > 1 && a[1] == "thread") {  // crt thread: start a thread that takes a msvcr100 _ptd (errno) and
                                             // waits; again: let it end (its _ptd freed by msvcr100's _freefls)
      static HANDLE th = nullptr, ev = nullptr;
      static uintptr_t ptd = 0;
      if (!th) {
        ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        th = CreateThread(nullptr, 0, [](LPVOID e) -> DWORD {
          HMODULE crt = GetModuleHandleW(L"msvcr100.dll");
          if (auto gp = crt ? (void*(__cdecl*)())GetProcAddress(crt, "_getptd") : nullptr) ptd = (uintptr_t)gp();
          WaitForSingleObject((HANDLE)e, INFINITE);
          return 0;
        }, ev, 0, nullptr);
        Sleep(200);
        out.push_back(Fmt("test thread %lu started, its msvcr100 _ptd %08X", GetThreadId(th), (unsigned)ptd));
      } else {
        SetEvent(ev);
        DWORD w = WaitForSingleObject(th, 5000);
        out.push_back(Fmt("test thread ended (%s)", w == WAIT_OBJECT_0 ? "ok" : "timeout"));
        CloseHandle(th), CloseHandle(ev);
        th = ev = nullptr;
      }
    }
    if (a.size() > 2 && a[1] == "add") {
      HMODULE crt = GetModuleHandleW(L"msvcr100.dll");
      auto ax = crt ? (int(__cdecl*)(void(__cdecl*)()))GetProcAddress(crt, "atexit") : nullptr;
      int n = atoi(a[2].c_str()), r = 0;
      for (int i = 0; ax && i < n; ++i) r += ax(+[]() {}) == 0;
      out.push_back(Fmt("atexit() x%d: %d ok", n, r));
    }
    out.push_back(OnexitCheck());
    out.push_back(g_flsWhy);
    uintptr_t crtHeap = 0;
    Heaps(&crtHeap);
    if (crtHeap) out.push_back(Fmt("msvcr100 heap %08X HeapValidate %d", (unsigned)crtHeap,
                                   (int)HeapValidate((HANDLE)crtHeap, 0, nullptr)));
    std::string crtMods;
    for (const ModRange& m : g_mods)
      if (m.crt) crtMods += " " + m.name;
    out.push_back("images in the snapshot as msvcr100 users:" + crtMods);
    return out;
  }
  if (c == "layout") return Layout(a.size() > 1 ? a[1] : "now");
  if (c == "disk") return DiskTest(arg(1, g_activeSlot + 1) - 1, a.size() > 2 && a[2] == "keep");
  if (c == "files") {
    std::vector<std::string> out{Fmt("game files: live %ld (zombies %ld), tracked since start %ld, closed at loads %ld",
                                     g_fileLive.load(), g_fileZombies.load(), g_fileTracked.load(),
                                     g_fileClosedAtLoad.load())};
    if (a.size() > 1 && a[1] == "list") {
      Spin lk(&g_fileLock);
      for (FileRec& r : g_files) {
        if (r.used != 1) continue;
        wchar_t path[MAX_PATH] = {};
        GetFinalPathNameByHandleW(r.h, path, MAX_PATH, 0);
        const wchar_t* fn = wcsrchr(path, L'\\');
        out.push_back(Fmt("  %p mask %u zombie %u pos %lld/%lld %s", r.h, r.mask, r.zombie, r.pos[0], r.pos[1],
                          util::WideToUtf8(fn ? fn + 1 : path).c_str()));
      }
    }
    return out;
  }
  if (c == "valve") {  // ss valve <free MB> <largest MB>
    if (a.size() > 2) g_valveTotalMB = strtoull(a[1].c_str(), nullptr, 10), g_valveBiggestMB = strtoull(a[2].c_str(), nullptr, 10);
    return {Fmt("low-memory valve: free < %llu MB or largest < %llu MB drops a slot", g_valveTotalMB, g_valveBiggestMB)};
  }
  if (c == "streamlog") {
    g_streamLog = arg(1, 1) != 0;
    return {Fmt("streaming log %d", (int)g_streamLog.load())};
  }
  if (c == "pack") {  // #29: ss pack 0|1 - pack slots (dedup + compression) or keep them raw (before #29, A/B)
    if (a.size() > 1) ssstore::SetEnabled(arg(1, 1) != 0);
    ssstore::WaitIdle(5000);
    ssstore::Stats st = ssstore::GetStats();
    return {Fmt("packing %d; store %.1f MB in %u chunks (live %.1f MB), %u entries; pages packed %llu: zero %llu, "
                "shared %llu, new %llu; compacted %.1f MB",
                (int)ssstore::Enabled(), st.chunkBytes / 1048576.0, st.chunks, st.liveBytes / 1048576.0, st.entries,
                st.packedPages, st.zeroPages, st.sharedPages, st.newPages, st.compactedBytes / 1048576.0),
            "last: " + st.last};
  }
  if (c == "cost") {  // #29: ss cost <MB> - what a slot of a new map opening is expected to keep (memory check)
    if (a.size() > 1) g_openingCostMB = strtoull(a[1].c_str(), nullptr, 10);
    return {Fmt("opening cost %llu MB (a save needs free >= %llu + 300 + this)", g_openingCostMB, g_valveTotalMB)};
  }
  if (c == "emptyall") {  // #29: = the tab's Empty all
    int before = g_done;
    Request(4, 0);
    for (int i = 0; i < 100 && g_done == before; ++i) Sleep(50);
    return {Message(), ProcMem()};
  }
  if (c == "thumb") {  // #29: ss thumb <slot> - writes the slot's picture as DmCPT_dumps\ss_thumb<N>.bmp
    int i = std::clamp(arg(1, 1) - 1, 0, kSlots - 1);
    std::lock_guard<std::mutex> lk(g_viewMu);
    const ThumbRec& t = g_thumbs[i];
    if (t.bgra.empty()) return {Fmt("slot %d has no picture", i + 1)};
    std::wstring path = util::ExeDir() + L"DmCPT_dumps\\ss_thumb" + std::to_wstring(i + 1) + L".bmp";
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return {"cannot write the picture"};
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    ih.biSize = sizeof ih, ih.biWidth = t.w, ih.biHeight = -t.h, ih.biPlanes = 1, ih.biBitCount = 32;
    fh.bfType = 0x4D42, fh.bfOffBits = sizeof fh + sizeof ih, fh.bfSize = fh.bfOffBits + (DWORD)t.bgra.size();
    fwrite(&fh, sizeof fh, 1, f);
    fwrite(&ih, sizeof ih, 1, f);
    fwrite(t.bgra.data(), 1, t.bgra.size(), f);
    fclose(f);
    return {Fmt("slot %d picture %dx%d (gen %u) -> %s", i + 1, t.w, t.h, t.gen, util::WideToUtf8(path).c_str())};
  }
  if (c == "slot") {
    SetActiveSlot(arg(1, 1) - 1);
    return {Fmt("active slot %d", g_activeSlot + 1)};
  }
  return {Fmt("armed %d (tick hook %d), active slot %d; %s", (int)g_armed.load(), (int)g_tickHooked, g_activeSlot + 1,
              Message().c_str()),
          "slot 1: " + SlotText(0), "slot 2: " + SlotText(1), "slot 3: " + SlotText(2), "slot 4: " + SlotText(3),
          "slot 5: " + SlotText(4),
          Fmt("game files live %ld (zombies %ld); rest sampler: %d threads, %ld samples; videos open %d (opened %ld, "
              "hook %s)", g_fileLive.load(), g_fileZombies.load(), g_restN.load(), g_restSamples.load(),
              g_binkOpen.load(), g_binkOpened.load(), oBinkOpen ? "on" : "NOT FOUND"),
          Fmt("D3D pins %zu (created %ld, dropped %ld, refs raised %ld); qpc offset %lld; %s", g_pins.size(),
              g_pinCreated.load(), g_pinDropped.load(), g_pinRefsAdded.load(), g_qpcOff.load(), ProcMem().c_str())};
}

}  // namespace savestate
