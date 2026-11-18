// #29 offline test of the save-state page store (src/savestate_store.cpp): pack two "slots" that share most pages,
// put them back into "game memory" that changed since, check every byte, time it.
//   store_test.exe [MB] [file ...]   (default 320 MB; files = page content, e.g. the game's exe and dlls)
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <random>
#include <set>
#include <vector>

#include "savestate_store.h"

using namespace ssstore;

static double Ms(LARGE_INTEGER a, LARGE_INTEGER b) {
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  return (b.QuadPart - a.QuadPart) * 1000.0 / f.QuadPart;
}
static LARGE_INTEGER Now() {
  LARGE_INTEGER x;
  QueryPerformanceCounter(&x);
  return x;
}

static HANDLE ToSection(const uint8_t* mem, uint64_t bytes) {
  HANDLE s = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT, (DWORD)(bytes >> 32),
                                (DWORD)bytes, nullptr);
  uint8_t* v = (uint8_t*)MapViewOfFile(s, FILE_MAP_WRITE, 0, 0, (SIZE_T)bytes);
  memcpy(v, mem, (size_t)bytes);
  UnmapViewOfFile(v);
  return s;
}

int main(int argc, char** argv) {
  size_t mb = argc > 1 ? (size_t)atoi(argv[1]) : 320;
  if (!mb) mb = 320;
  const size_t pages = mb * 256, bytes = pages * kPage;
  std::vector<uint8_t> files;
  for (int i = 2; i < argc; ++i) {
    FILE* f = fopen(argv[i], "rb");
    if (!f) continue;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    size_t at = files.size();
    files.resize(at + n);
    fread(files.data() + at, 1, n, f);
    fclose(f);
  }
  std::mt19937 rng(29);
  uint8_t* A = (uint8_t*)VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  uint8_t* B = (uint8_t*)VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  uint8_t* G = (uint8_t*)VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
  if (!A || !B || !G) {
    printf("FAIL: no memory for %zu MB x3\n", mb);
    return 1;
  }
  // A: file pages, structured pages (floats / pointers), random pages, 12% zero pages, some duplicates
  for (size_t p = 0; p < pages; ++p) {
    uint8_t* d = A + p * kPage;
    unsigned k = rng() % 100;
    if (k < 12) continue;  // zero
    if (k < 60 && files.size() > kPage) {
      size_t off = ((size_t)rng() % (files.size() / kPage)) * kPage;
      memcpy(d, files.data() + off, kPage);
    } else if (k < 85) {
      float* f = (float*)d;
      uint32_t base = 0x10000000u + (rng() % 0x1000) * 0x1000;
      for (int i = 0; i < 1024; i += 4) {
        f[i] = (float)(rng() % 1000) * 0.5f;
        f[i + 1] = 1.0f;
        ((uint32_t*)f)[i + 2] = base + (rng() % 64) * 16;
        ((uint32_t*)f)[i + 3] = 0;
      }
    } else if (k < 92 && p > 0) {
      memcpy(d, A + (rng() % p) * kPage, kPage);  // a duplicate of an earlier page
    } else {
      for (int i = 0; i < 1024; ++i) ((uint32_t*)d)[i] = rng();
    }
  }
  // B: A with 10% of the pages changed (another slot of the same mission)
  memcpy(B, A, bytes);
  for (size_t p = 0; p < pages; ++p)
    if (rng() % 10 == 0) ((uint32_t*)(B + p * kPage))[rng() % 1024] ^= 0x5A5A5A5Au;

  // hash quality: distinct contents vs distinct hashes
  {
    std::set<uint64_t> hs;
    LARGE_INTEGER t0 = Now();
    for (size_t p = 0; p < pages; ++p) hs.insert(PageHash(A + p * kPage));
    LARGE_INTEGER t1 = Now();
    uint64_t sum = 0;
    for (size_t p = 0; p < pages; ++p) sum += PageHash(A + p * kPage);
    LARGE_INTEGER t2 = Now();
    printf("hash: %zu pages, %zu distinct hashes; %.0f ms for %zu MB (%.1f GB/s) [%llx]\n", pages, hs.size(),
           Ms(t1, t2), mb, mb / 1024.0 / (Ms(t1, t2) / 1000.0), (unsigned long long)sum);
    (void)t0;
  }
  // collision check: one-bit changes and swapped stripes give other hashes
  {
    int bad = 0;
    alignas(16) static uint8_t pg[kPage], q[kPage];
    for (int t = 0; t < 20000; ++t) {
      memcpy(pg, A + (rng() % pages) * kPage, kPage);
      memcpy(q, pg, kPage);
      if (t & 1) {
        q[rng() % kPage] ^= (uint8_t)(1u << (rng() % 8));
      } else {
        int a = rng() % 64, b = rng() % 64;
        if (a == b || !memcmp(pg + a * 64, pg + b * 64, 64)) continue;
        uint8_t tmp[64];
        memcpy(tmp, q + a * 64, 64);
        memcpy(q + a * 64, q + b * 64, 64);
        memcpy(q + b * 64, tmp, 64);
      }
      if (memcmp(pg, q, kPage) && PageHash(pg) == PageHash(q)) ++bad;
    }
    printf("hash: changed/swapped pages with the same hash: %d of 20000 (want 0)\n", bad);
    if (bad) return 1;
  }

  Start();
  PageList pl1, pl2;
  LARGE_INTEGER t0 = Now();
  Pack(ToSection(A, bytes), pages, [&](PageList&& p) { pl1 = std::move(p); return true; }, "slot 1");
  WaitIdle(120000);
  LARGE_INTEGER t1 = Now();
  printf("%s\n", GetStats().last.c_str());
  Pack(ToSection(B, bytes), pages, [&](PageList&& p) { pl2 = std::move(p); return true; }, "slot 2");
  WaitIdle(120000);
  LARGE_INTEGER t2 = Now();
  printf("%s\n", GetStats().last.c_str());
  printf("pack wall: slot 1 %.0f ms, slot 2 %.0f ms\n", Ms(t0, t1), Ms(t1, t2));
  if (pl1.size() != pages || pl2.size() != pages) {
    printf("FAIL: not packed (%zu %zu)\n", pl1.size(), pl2.size());
    return 1;
  }
  PrepareRestore(pages);
  std::vector<Block> blocks;
  for (size_t p = 0; p < pages; p += 32)
    blocks.push_back({(uintptr_t)(G + p * kPage), (uint32_t)p, (uint32_t)std::min<size_t>(32, pages - p)});
  auto check = [&](const char* what, const uint8_t* want, int changePct) {
    memcpy(G, B == want ? A : B, bytes);  // start from the other slot
    size_t touched = 0;
    for (size_t p = 0; p < pages; ++p) {
      unsigned r = rng() % 100;
      if ((int)r < changePct) {
        uint8_t* d = G + p * kPage;
        if (r % 3 == 0) memset(d, 0, kPage);
        else for (int i = 0; i < 64; ++i) ((uint32_t*)d)[rng() % 1024] = rng();
        ++touched;
      }
    }
    std::unique_lock<std::recursive_mutex> lk(Mu());
    RestoreStats st = Restore(want == A ? pl1 : pl2, blocks.data(), blocks.size());
    lk.unlock();
    size_t bad = 0;
    for (size_t p = 0; p < pages; ++p) bad += memcmp(G + p * kPage, want + p * kPage, kPage) != 0;
    printf("restore %s (%d%% changed, %zu pages touched): %u pages, %u changed, %u zeroed, %.1f ms on %d threads, "
           "ok %d, bad pages %zu\n",
           what, changePct, touched, st.pages, st.changed, st.zeroed, st.ms, st.threads, (int)st.ok, bad);
    return bad == 0 && st.ok;
  };
  bool ok = check("slot 1", A, 5) && check("slot 2", B, 5) && check("slot 1", A, 50) && check("slot 2", B, 0);
  {  // no change at all: the hash pass only
    std::unique_lock<std::recursive_mutex> lk(Mu());
    RestoreStats st = Restore(pl2, blocks.data(), blocks.size());
    printf("restore again, nothing changed: %u changed, %.1f ms\n", st.changed, st.ms);
    ok &= st.changed == 0 && st.zeroed == 0;
  }
  {
    std::unique_lock<std::recursive_mutex> lk(Mu());
    Release(pl1);
  }
  Stats s = GetStats();
  printf("after releasing slot 1: store %.1f MB in %u chunks, live %.1f MB, %u entries\n", s.chunkBytes / 1048576.0,
         s.chunks, s.liveBytes / 1048576.0, s.entries);
  ok &= check("slot 2 after slot 1 is gone", B, 20);
  {
    std::unique_lock<std::recursive_mutex> lk(Mu());
    Release(pl2);
  }
  s = GetStats();
  printf("after releasing both: store %.1f MB in %u chunks, %u entries\n", s.chunkBytes / 1048576.0, s.chunks,
         s.entries);
  ok &= s.entries == 0;
  printf("%s\n", ok ? "ALL OK" : "FAILED");
  return ok ? 0 : 1;
}
