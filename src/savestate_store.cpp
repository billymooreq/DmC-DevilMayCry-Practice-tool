#include "savestate_store.h"

#include <emmintrin.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <unordered_map>

namespace ssstore {
namespace {

// ---- hash: XXH3-style 64-bit over a 4 KB page (SSE2: 32x32->64 products, 4 x 128-bit lanes, a key that moves per
// 64-byte stripe, a scramble per 1 KB block, an avalanche at the end). Used to find shared pages (checked byte by
// byte before sharing) and, on a load, to skip pages that are as saved.
alignas(16) const uint8_t kSecret[192] = {
    0xb8, 0xfe, 0x6c, 0x39, 0x23, 0xa4, 0x4b, 0xbe, 0x7c, 0x01, 0x81, 0x2c, 0xf7, 0x21, 0xad, 0x1c, 0xde, 0xd4, 0x6d,
    0xe9, 0x83, 0x90, 0x97, 0xdb, 0x72, 0x40, 0xa4, 0xa4, 0xb7, 0xb3, 0x67, 0x1f, 0xcb, 0x79, 0xe6, 0x4e, 0xcc, 0xc0,
    0xe5, 0x78, 0x82, 0x5a, 0xd0, 0x7d, 0xcc, 0xff, 0x72, 0x21, 0xb8, 0x08, 0x46, 0x74, 0xf7, 0x43, 0x24, 0x8e, 0xe0,
    0x35, 0x90, 0xe6, 0x81, 0x3a, 0x26, 0x4c, 0x3c, 0x28, 0x52, 0xbb, 0x91, 0xc3, 0x00, 0xcb, 0x88, 0xd0, 0x65, 0x8b,
    0x1b, 0x53, 0x2e, 0xa3, 0x71, 0x64, 0x48, 0x97, 0xa2, 0x0d, 0xf9, 0x4e, 0x38, 0x19, 0xef, 0x46, 0xa9, 0xde, 0xac,
    0xd8, 0xa8, 0xfa, 0x76, 0x3f, 0xe3, 0x9c, 0x34, 0x3f, 0xf9, 0xdc, 0xbb, 0xc7, 0xc7, 0x0b, 0x4f, 0x1d, 0x8a, 0x51,
    0xe0, 0x4b, 0xcd, 0xb4, 0x59, 0x31, 0xc8, 0x9f, 0x7e, 0xc9, 0xd9, 0x78, 0x73, 0x64, 0xea, 0xc5, 0xac, 0x83, 0x34,
    0xd3, 0xeb, 0xc3, 0xc5, 0x81, 0xa0, 0xff, 0xfa, 0x13, 0x63, 0xeb, 0x17, 0x0d, 0xdd, 0x51, 0xb7, 0xf0, 0xda, 0x49,
    0xd3, 0x16, 0x55, 0x26, 0x29, 0xd4, 0x68, 0x9e, 0x2b, 0x16, 0xbe, 0x58, 0x7d, 0x47, 0xa1, 0xfc, 0x8f, 0xf8, 0xb8,
    0xd1, 0x7a, 0xd0, 0x31, 0xce, 0x45, 0xcb, 0x3a, 0x8f, 0x95, 0x16, 0x04, 0x28, 0xaf, 0xd7, 0xfb, 0xca, 0xbb, 0x4b,
    0x40, 0x7e,
};

inline uint64_t Fmix(uint64_t h) {
  h ^= h >> 33;
  h *= 0xff51afd7ed558ccdull;
  h ^= h >> 33;
  h *= 0xc4ceb9fe1a85ec53ull;
  h ^= h >> 33;
  return h;
}

// ---- compression (ntdll, XPRESS: plain LZ77, fast both ways)
using Comp_t = LONG(NTAPI*)(USHORT, PUCHAR, ULONG, PUCHAR, ULONG, ULONG, PULONG, PVOID);
using Decomp_t = LONG(NTAPI*)(USHORT, PUCHAR, ULONG, PUCHAR, ULONG, PULONG, PVOID);
using Ws_t = LONG(NTAPI*)(USHORT, PULONG, PULONG);
constexpr USHORT kFmt = 3;  // COMPRESSION_FORMAT_XPRESS
Comp_t g_comp = nullptr;
Decomp_t g_decomp = nullptr;
ULONG g_wsSize = 0;

// ---- the store
constexpr uint32_t kChunk = 4u << 20;  // one page-file section; a reader maps it whole
constexpr int kMaxChunks = 2048;        // 8 GB
struct Chunk {
  HANDLE sec = nullptr;
  uint32_t used = 0;  // bytes appended
  uint32_t live = 0;  // bytes of entries still referenced
};
Chunk g_chunks[kMaxChunks];  // fixed array: never moves under a reader
struct Entry {
  uint64_t hash;
  uint32_t refs;  // 0 = free
  uint32_t off;
  uint16_t chunk;
  uint16_t size;  // kPage = stored as is
};
std::vector<Entry> g_e;
std::vector<uint32_t> g_free;
std::unordered_multimap<uint64_t, uint32_t> g_byHash;  // hash -> entry index
std::recursive_mutex g_mu;
int g_cur = -1;               // chunk the packer appends to
uint8_t* g_curView = nullptr;  // its writable view (packer thread)
std::atomic<bool> g_enabled{true};
Stats g_stats;  // under g_mu

// ---- packer
struct Job {
  HANDLE sec;
  uint64_t pages;
  std::function<bool(PageList&&)> done;
  std::string tag;
};
std::mutex g_jobMu;
std::deque<Job> g_jobs;
HANDLE g_jobEv = nullptr;
std::atomic<int> g_busy{0};

double Ms(LARGE_INTEGER a, LARGE_INTEGER b) {
  static LARGE_INTEGER f = [] {
    LARGE_INTEGER x;
    QueryPerformanceFrequency(&x);
    return x;
  }();
  return (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
}
LARGE_INTEGER Now() {
  LARGE_INTEGER x;
  QueryPerformanceCounter(&x);
  return x;
}

// A small cache of read-only chunk views (one per thread that reads the store).
struct ViewCache {
  int chunk[3] = {-1, -1, -1};
  uint8_t* p[3] = {};
  unsigned tick[3] = {};
  unsigned t = 0;
  const uint8_t* Get(int c) {
    ++t;
    for (int i = 0; i < 3; ++i)
      if (chunk[i] == c) {
        tick[i] = t;
        return p[i];
      }
    int v = 0;
    for (int i = 1; i < 3; ++i)
      if (tick[i] < tick[v]) v = i;
    if (p[v]) UnmapViewOfFile(p[v]);
    p[v] = (uint8_t*)MapViewOfFile(g_chunks[c].sec, FILE_MAP_READ, 0, 0, kChunk);
    chunk[v] = p[v] ? c : -1;
    tick[v] = t;
    return p[v];
  }
  void Drop(int c) {  // a chunk closed / moved
    for (int i = 0; i < 3; ++i)
      if (chunk[i] == c) {
        if (p[i]) UnmapViewOfFile(p[i]);
        p[i] = nullptr;
        chunk[i] = -1;
      }
  }
  void Clear() {
    for (int i = 0; i < 3; ++i) {
      if (p[i]) UnmapViewOfFile(p[i]);
      p[i] = nullptr;
      chunk[i] = -1;
    }
  }
};
ViewCache g_packView;  // packer thread (under g_mu)

const uint8_t* EntryData(ViewCache& vc, const Entry& e) {
  if ((int)e.chunk == g_cur && g_curView) return g_curView + e.off;
  const uint8_t* v = vc.Get(e.chunk);
  return v ? v + e.off : nullptr;
}

bool Unpack(const Entry& e, const uint8_t* src, void* dst, void* ws) {
  if (e.size == kPage) {
    memcpy(dst, src, kPage);
    return true;
  }
  ULONG fin = 0;
  return g_decomp(kFmt, (PUCHAR)dst, kPage, (PUCHAR)src, e.size, &fin, ws) >= 0 && fin == kPage;
}

void CloseChunk(int c) {
  g_packView.Drop(c);
  if (g_chunks[c].sec) CloseHandle(g_chunks[c].sec);
  g_chunks[c] = {};
}

// g_mu held. A new chunk to append to.
bool NewChunk() {
  if (g_curView) UnmapViewOfFile(g_curView);
  g_curView = nullptr;
  int old = g_cur;
  g_cur = -1;
  if (old >= 0 && g_chunks[old].live == 0) CloseChunk(old);
  for (int c = 0; c < kMaxChunks; ++c)
    if (!g_chunks[c].sec) {
      HANDLE s = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, 0, kChunk, nullptr);
      if (!s) return false;
      uint8_t* v = (uint8_t*)MapViewOfFile(s, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, kChunk);
      if (!v) {
        CloseHandle(s);
        return false;
      }
      g_chunks[c] = {s, 0, 0};
      g_cur = c;
      g_curView = v;
      return true;
    }
  return false;
}

// g_mu held. Index + 1 of a new entry, 0 = no memory.
uint32_t Append(uint64_t h, const uint8_t* data, uint32_t size) {
  if (g_cur < 0 || g_chunks[g_cur].used + size > kChunk)
    if (!NewChunk()) return 0;
  Chunk& c = g_chunks[g_cur];
  memcpy(g_curView + c.used, data, size);
  Entry e{h, 1, c.used, (uint16_t)g_cur, (uint16_t)size};
  c.used += size;
  c.live += size;
  uint32_t i;
  if (!g_free.empty()) {
    i = g_free.back();
    g_free.pop_back();
    g_e[i] = e;
  } else {
    i = (uint32_t)g_e.size();
    g_e.push_back(e);
  }
  g_byHash.emplace(h, i);
  return i + 1;
}

void Unref(uint32_t idx) {  // g_mu held
  Entry& e = g_e[idx - 1];
  if (!e.refs || --e.refs) return;
  Chunk& c = g_chunks[e.chunk];
  c.live -= e.size;
  auto r = g_byHash.equal_range(e.hash);
  for (auto it = r.first; it != r.second; ++it)
    if (it->second == idx - 1) {
      g_byHash.erase(it);
      break;
    }
  g_free.push_back(idx - 1);
  if (c.live == 0 && (int)e.chunk != g_cur) CloseChunk(e.chunk);
}

// Garbage: chunks mostly made of released entries are copied into the current chunk and closed (packer thread, after
// a job). One chunk per lock.
void Compact() {
  for (int round = 0; round < 64; ++round) {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    uint64_t used = 0, live = 0;
    int worst = -1;
    double wr = 1.0;
    for (int c = 0; c < kMaxChunks; ++c) {
      if (!g_chunks[c].sec) continue;
      used += g_chunks[c].used;
      live += g_chunks[c].live;
      if (c == g_cur || !g_chunks[c].used) continue;
      double r = (double)g_chunks[c].live / g_chunks[c].used;
      if (r < wr) wr = r, worst = c;
    }
    if (worst < 0 || used - live < (96ull << 20) || wr > 0.5) return;
    const uint8_t* src = g_packView.Get(worst);
    if (!src) return;
    uint32_t moved = 0;
    for (uint32_t i = 0; i < g_e.size(); ++i) {
      Entry& e = g_e[i];
      if (!e.refs || (int)e.chunk != worst) continue;
      if (g_cur < 0 || g_chunks[g_cur].used + e.size > kChunk) {
        if (!NewChunk()) return;
        src = g_packView.Get(worst);  // NewChunk may close a chunk: views of others stay
        if (!src) return;
      }
      Chunk& c = g_chunks[g_cur];
      memcpy(g_curView + c.used, src + e.off, e.size);
      g_chunks[worst].live -= e.size;
      e.chunk = (uint16_t)g_cur;
      e.off = c.used;
      c.used += e.size;
      c.live += e.size;
      moved += e.size;
    }
    g_stats.compactedBytes += moved;
    CloseChunk(worst);
  }
}

void RunJob(Job& j) {
  LARGE_INTEGER t0 = Now();
  PageList pl;
  pl.resize((size_t)j.pages);
  static std::vector<uint8_t> buf(kPage * 2), tmp(kPage), ws;
  if (ws.empty()) ws.resize(g_wsSize + 64);
  constexpr uint64_t kView = 64ull << 20;
  const uint64_t total = j.pages * kPage;
  uint8_t* view = nullptr;
  uint64_t viewOff = ~0ull;
  uint64_t zero = 0, shared = 0, fresh = 0, compBytes = 0;
  bool ok = true;
  for (uint64_t i = 0; i < j.pages && ok; ++i) {
    uint64_t off = i * kPage, vo = off & ~(kView - 1);
    if (vo != viewOff) {
      if (view) UnmapViewOfFile(view);
      view = (uint8_t*)MapViewOfFile(j.sec, FILE_MAP_READ, (DWORD)(vo >> 32), (DWORD)vo,
                                     (SIZE_T)std::min<uint64_t>(kView, total - vo));
      viewOff = vo;
      if (!view) {
        ok = false;
        break;
      }
    }
    const uint8_t* p = view + (off - vo);
    if (IsZero(p)) {
      pl[(size_t)i] = 0;
      ++zero;
      continue;
    }
    uint64_t h = PageHash(p);
    uint32_t idx = 0;
    {
      std::lock_guard<std::recursive_mutex> lk(g_mu);
      auto r = g_byHash.equal_range(h);
      for (auto it = r.first; it != r.second && !idx; ++it) {
        Entry& e = g_e[it->second];
        const uint8_t* src = EntryData(g_packView, e);
        if (src && Unpack(e, src, tmp.data(), ws.data()) && memcmp(tmp.data(), p, kPage) == 0) {
          ++e.refs;
          idx = it->second + 1;
        }
      }
    }
    if (idx) {
      pl[(size_t)i] = idx;
      ++shared;
      continue;
    }
    ULONG cs = 0;
    const uint8_t* data = buf.data();
    if (g_comp(kFmt, (PUCHAR)p, kPage, buf.data(), (ULONG)buf.size(), 4096, &cs, ws.data()) < 0 || cs == 0 ||
        cs >= kPage - 64)
      cs = kPage, data = p;  // incompressible: stored as is
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    idx = Append(h, data, cs);
    if (!idx) {
      ok = false;
      break;
    }
    pl[(size_t)i] = idx;
    ++fresh;
    compBytes += cs;
  }
  if (view) UnmapViewOfFile(view);
  CloseHandle(j.sec);
  bool taken = false;
  char line[400];
  {
    std::lock_guard<std::recursive_mutex> lk(g_mu);
    if (ok) taken = j.done(std::move(pl));
    if (!taken) {  // the slot changed meanwhile (or no memory: the pages stay raw)
      for (uint32_t idx : pl)
        if (idx) Unref(idx);
    }
    double ms = Ms(t0, Now());
    uint64_t cb = 0, live = 0;
    uint32_t n = 0;
    for (const Chunk& c : g_chunks)
      if (c.sec) cb += kChunk, live += c.live, ++n;
    snprintf(line, sizeof line,
             "pack %s: %llu pages (%.1f MB): zero %llu, shared %llu, new %llu -> %.1f MB compressed; %.0f ms; %s; "
             "store %.1f MB in %u chunks (live %.1f MB), %zu entries",
             j.tag.c_str(), (unsigned long long)j.pages, j.pages * kPage / 1048576.0, (unsigned long long)zero,
             (unsigned long long)shared, (unsigned long long)fresh, compBytes / 1048576.0, ms,
             !ok ? "FAILED (no memory) - the slot stays raw" : taken ? "taken" : "dropped (the slot changed)",
             cb / 1048576.0, n, live / 1048576.0, g_e.size() - g_free.size());
    g_stats.packMs = ms;
    g_stats.packedPages += j.pages;
    g_stats.zeroPages += zero;
    g_stats.sharedPages += shared;
    g_stats.newPages += fresh;
    g_stats.last = line;
  }
  pl.clear();
  pl.shrink_to_fit();
  Compact();
}

DWORD WINAPI PackerThread(LPVOID) {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
  for (;;) {
    WaitForSingleObject(g_jobEv, INFINITE);
    for (;;) {
      Job j;
      {
        std::lock_guard<std::mutex> lk(g_jobMu);
        if (g_jobs.empty()) break;
        j = std::move(g_jobs.front());
        g_jobs.pop_front();
      }
      RunJob(j);
      --g_busy;
    }
  }
}

// ---- the crew (load). Phase 1: every page hashed against the save (zero pages zeroed at once), the changed ones
// listed; the lists are merged and sorted by store chunk. Phase 2: one chunk at a time per worker - each chunk is
// mapped once per load. No heap use (PrepareRestore sized everything before the pause).
constexpr int kMaxCrew = 8;
struct Change {
  uint64_t key;  // chunk << 32 | offset
  uintptr_t dst;
  uint32_t idx;
};
struct Worker {
  HANDLE th = nullptr, go = nullptr, done = nullptr;
  std::vector<uint8_t> ws;
  std::vector<Change> changes;  // phase 1 output (capacity reserved)
  size_t nChanges = 0;
  uint32_t zeroed = 0;
  bool bad = false, overflow = false;
};
Worker g_w[kMaxCrew + 1];  // the last one = the calling (game) thread
int g_crew = 0;
const PageList* g_jobPl = nullptr;
const Block* g_jobBlocks = nullptr;
long g_jobN = 0;
int g_phase = 0;
std::atomic<long> g_next{0};
std::vector<Change> g_all;          // merged (capacity reserved)
std::vector<uint32_t> g_groupAt;    // start of each chunk's run in g_all (+ end)
size_t g_nAll = 0, g_nGroups = 0;

void Phase1(Worker& w) {
  w.nChanges = 0;
  w.zeroed = 0;
  w.bad = w.overflow = false;
  const PageList& pl = *g_jobPl;
  for (long b; (b = g_next++) < g_jobN;) {
    const Block& k = g_jobBlocks[b];
    for (uint32_t j = 0; j < k.n; ++j) {
      uint8_t* page = (uint8_t*)(k.addr + (uintptr_t)j * kPage);
      uint32_t idx = pl[k.first + j];
      if (!idx) {
        if (!IsZero(page)) {
          memset(page, 0, kPage);
          ++w.zeroed;
        }
        continue;
      }
      const Entry& e = g_e[idx - 1];
      if (PageHash(page) == e.hash) continue;
      if (w.nChanges == w.changes.size()) {
        w.overflow = true;
        continue;
      }
      w.changes[w.nChanges++] = {((uint64_t)e.chunk << 32) | e.off, (uintptr_t)page, idx};
    }
  }
}

void Phase2(Worker& w) {
  for (long g; (g = g_next++) < (long)g_nGroups;) {
    uint32_t a = g_groupAt[g], b = g_groupAt[g + 1];
    int c = (int)(g_all[a].key >> 32);
    const uint8_t* v = (const uint8_t*)MapViewOfFile(g_chunks[c].sec, FILE_MAP_READ, 0, 0, kChunk);
    if (!v) {
      w.bad = true;
      continue;
    }
    for (uint32_t i = a; i < b; ++i) {
      const Entry& e = g_e[g_all[i].idx - 1];
      if (!Unpack(e, v + e.off, (void*)g_all[i].dst, w.ws.data())) w.bad = true;
    }
    UnmapViewOfFile(v);
  }
}

void Work(Worker& w) {
  if (g_phase == 1) Phase1(w);
  else Phase2(w);
}

DWORD WINAPI CrewThread(LPVOID p) {
  Worker& w = *(Worker*)p;
  for (;;) {
    WaitForSingleObject(w.go, INFINITE);
    Work(w);
    SetEvent(w.done);
  }
}

void RunPhase(int phase) {
  g_phase = phase;
  g_next = 0;
  for (int i = 0; i < g_crew; ++i) SetEvent(g_w[i].go);
  Work(g_w[kMaxCrew]);
  HANDLE wait[kMaxCrew];
  for (int i = 0; i < g_crew; ++i) wait[i] = g_w[i].done;
  if (g_crew) WaitForMultipleObjects(g_crew, wait, TRUE, INFINITE);
}

}  // namespace

uint64_t PageHash(const void* page) {
  const __m128i* d = (const __m128i*)page;
  __m128i acc[4] = {_mm_set_epi32(0, 0x3C6EF372, 0, (int)0xC2B2AE3D), _mm_set_epi32(0, (int)0x9E3779B1, 0, 0x165667B1),
                    _mm_set_epi32(0, (int)0x85EBCA77, 0, 0x27D4EB2F), _mm_set_epi32(0, 0x61C88647, 0, (int)0xBB67AE85)};
  const __m128i prime = _mm_set1_epi32((int)0x9E3779B1);
  for (int blk = 0; blk < 4; ++blk) {      // 4 blocks of 1 KB
    for (int s = 0; s < 16; ++s) {         // 16 stripes of 64 B
      const uint8_t* key = kSecret + s * 8;
      for (int l = 0; l < 4; ++l) {
        __m128i dv = _mm_load_si128(d + blk * 64 + s * 4 + l);
        __m128i kv = _mm_loadu_si128((const __m128i*)(key + l * 16));
        __m128i dk = _mm_xor_si128(dv, kv);
        __m128i dkLo = _mm_shuffle_epi32(dk, _MM_SHUFFLE(0, 3, 0, 1));
        __m128i prod = _mm_mul_epu32(dk, dkLo);
        __m128i sw = _mm_shuffle_epi32(dv, _MM_SHUFFLE(1, 0, 3, 2));
        acc[l] = _mm_add_epi64(acc[l], _mm_add_epi64(prod, sw));
      }
    }
    for (int l = 0; l < 4; ++l) {  // scramble
      __m128i a = _mm_xor_si128(acc[l], _mm_srli_epi64(acc[l], 47));
      __m128i k = _mm_loadu_si128((const __m128i*)(kSecret + 128 + l * 16));
      __m128i dk = _mm_xor_si128(a, k);
      __m128i dkHi = _mm_shuffle_epi32(dk, _MM_SHUFFLE(0, 3, 0, 1));
      __m128i lo = _mm_mul_epu32(dk, prime);
      __m128i hi = _mm_mul_epu32(dkHi, prime);
      acc[l] = _mm_add_epi64(lo, _mm_slli_epi64(hi, 32));
    }
  }
  alignas(16) uint64_t v[8];
  for (int l = 0; l < 4; ++l) _mm_store_si128((__m128i*)(v + l * 2), acc[l]);
  uint64_t h = 0x27D4EB2F165667C5ull;
  for (int i = 0; i < 8; ++i) h = Fmix(h ^ (v[i] + 0x9E3779B97F4A7C15ull * (i + 1)));
  return h;
}

bool IsZero(const void* page) {
  const __m128i* d = (const __m128i*)page;
  __m128i a = _mm_setzero_si128(), b = _mm_setzero_si128();
  for (int i = 0; i < 256; i += 2) {
    a = _mm_or_si128(a, _mm_load_si128(d + i));
    b = _mm_or_si128(b, _mm_load_si128(d + i + 1));
  }
  return _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_or_si128(a, b), _mm_setzero_si128())) == 0xFFFF;
}

void Start() {
  static bool done = false;
  if (done) return;
  done = true;
  HMODULE nt = GetModuleHandleW(L"ntdll.dll");
  g_comp = (Comp_t)GetProcAddress(nt, "RtlCompressBuffer");
  g_decomp = (Decomp_t)GetProcAddress(nt, "RtlDecompressBufferEx");
  auto wsz = (Ws_t)GetProcAddress(nt, "RtlGetCompressionWorkSpaceSize");
  ULONG a = 0, b = 0;
  if (!g_comp || !g_decomp || !wsz || wsz(kFmt, &a, &b) < 0) {
    g_enabled = false;  // no compression: slots stay raw
    return;
  }
  g_wsSize = std::max(a, b);
  g_e.reserve(1 << 18);
  g_byHash.reserve(1 << 18);
  g_jobEv = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (HANDLE t = CreateThread(nullptr, 0, &PackerThread, nullptr, 0, nullptr)) CloseHandle(t);
  SYSTEM_INFO si{};
  GetSystemInfo(&si);
  g_crew = std::max(0, std::min<int>(kMaxCrew, (int)si.dwNumberOfProcessors - 2));
  for (int i = 0; i <= kMaxCrew; ++i) g_w[i].ws.resize(g_wsSize + 64);
  for (int i = 0; i < g_crew; ++i) {
    g_w[i].go = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_w[i].done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    g_w[i].th = CreateThread(nullptr, 0, &CrewThread, &g_w[i], 0, nullptr);
    if (!g_w[i].th) {
      g_crew = i;
      break;
    }
  }
}

std::recursive_mutex& Mu() { return g_mu; }

void Pack(HANDLE sec, uint64_t pages, std::function<bool(PageList&&)> done, std::string tag) {
  ++g_busy;
  {
    std::lock_guard<std::mutex> lk(g_jobMu);
    g_jobs.push_back({sec, pages, std::move(done), std::move(tag)});
  }
  SetEvent(g_jobEv);
}

bool Packing() { return g_busy.load() > 0; }

void WaitIdle(int ms) {
  for (int i = 0; i < ms / 10 && Packing(); ++i) Sleep(10);
}

void Release(PageList& pl) {
  for (uint32_t idx : pl)
    if (idx) Unref(idx);
  PageList().swap(pl);
}

void PrepareRestore(size_t pages) {
  for (int i = 0; i <= kMaxCrew; ++i)
    if (g_w[i].changes.size() < pages) g_w[i].changes.resize(pages);
  if (g_all.size() < pages) g_all.resize(pages);
  if (g_groupAt.size() < (size_t)kMaxChunks + 1) g_groupAt.resize(kMaxChunks + 1);
}

RestoreStats Restore(const PageList& pl, const Block* blocks, size_t nBlocks) {
  RestoreStats st;
  LARGE_INTEGER t0 = Now();
  g_jobPl = &pl;
  g_jobBlocks = blocks;
  g_jobN = (long)nBlocks;
  RunPhase(1);
  LARGE_INTEGER t1 = Now();
  g_nAll = 0;
  for (int i = 0; i <= kMaxCrew; ++i) {
    Worker& w = g_w[i];
    if (i < kMaxCrew && i >= g_crew) continue;
    st.zeroed += w.zeroed;
    if (w.bad || w.overflow) st.ok = false;
    for (size_t k = 0; k < w.nChanges && g_nAll < g_all.size(); ++k) g_all[g_nAll++] = w.changes[k];
  }
  std::sort(g_all.begin(), g_all.begin() + g_nAll, [](const Change& a, const Change& b) { return a.key < b.key; });
  g_nGroups = 0;
  for (size_t i = 0; i < g_nAll; ++i)
    if (i == 0 || (g_all[i].key >> 32) != (g_all[i - 1].key >> 32)) g_groupAt[g_nGroups++] = (uint32_t)i;
  g_groupAt[g_nGroups] = (uint32_t)g_nAll;
  RunPhase(2);
  for (int i = 0; i < g_crew; ++i) st.ok &= !g_w[i].bad;
  st.ok &= !g_w[kMaxCrew].bad;
  st.changed = (uint32_t)g_nAll;
  for (size_t b = 0; b < nBlocks; ++b) st.pages += blocks[b].n;
  st.threads = g_crew + 1;
  st.hashMs = Ms(t0, t1);
  st.ms = Ms(t0, Now());
  return st;
}

Stats GetStats() {
  std::lock_guard<std::recursive_mutex> lk(g_mu);
  Stats s = g_stats;
  for (const Chunk& c : g_chunks)
    if (c.sec) s.chunkBytes += kChunk, s.liveBytes += c.live, ++s.chunks;
  s.entries = (uint32_t)(g_e.size() - g_free.size());
  return s;
}

void SetEnabled(bool on) { g_enabled = on && g_comp; }
bool Enabled() { return g_enabled; }

}  // namespace ssstore
