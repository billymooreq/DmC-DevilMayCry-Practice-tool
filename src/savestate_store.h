#pragma once
#include <windows.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// #29: the page store of the save states (savestate.cpp). A save copies the game's memory into a page-file section
// of its own while the game is paused (the fastest copy, as before). The packer thread then packs that copy: zero
// pages are dropped, a page whose content is in the store already (any slot - slots of one mission share ~90% of
// their pages) is shared, every other page is compressed on its own (XPRESS) into page-file chunks; the raw copy is
// closed. A load of a packed slot hashes every live page and decompresses only the pages that differ from the save
// (game paused; a crew of the tool's threads does it in parallel). No game code is involved: tests run offline.
namespace ssstore {

constexpr uint32_t kPage = 4096;
// A slot's pages, in the order of its raw copy: 0 = a zero page, else 1 + index of a stored page.
using PageList = std::vector<uint32_t>;

void Start();  // once: work spaces, the crew and the packer thread (idle until used)
// Held by the load from before its pause to after it, and by every change of the store or of a slot's pages /
// raw copy. The packer takes it per page; it never waits on the game.
std::recursive_mutex& Mu();

// Queue a raw copy (pages * kPage bytes of section sec; the store closes sec when done). When packed, done() is
// called on the packer thread with Mu() held: true = the slot took the pages, false = the slot changed meanwhile
// (the store releases them). A failure (no memory) calls done with an empty list... never: the pages stay raw.
void Pack(HANDLE sec, uint64_t pages, std::function<bool(PageList&&)> done, std::string tag);
bool Packing();          // a job is queued or running
void WaitIdle(int ms);   // tests: until the queue is empty (or ms)

void Release(PageList& pl);  // Mu() held; never while the game is paused (frees heap memory)

// Load (Mu() held, the game may be paused: no heap use inside). Blocks of pages to put back: addr = the first
// page in the game, first = its index in pl, n pages. Pages equal to the save (hash) are left alone.
struct Block {
  uintptr_t addr;
  uint32_t first;
  uint32_t n;
};
struct RestoreStats {
  uint32_t pages = 0, changed = 0, zeroed = 0;
  double ms = 0, hashMs = 0;
  int threads = 0;
  bool ok = true;
};
// Before the pause (heap): room for a load of up to `pages` pages.
void PrepareRestore(size_t pages);
RestoreStats Restore(const PageList& pl, const Block* blocks, size_t nBlocks);

// A raw page compared and copied back (the slot is not packed yet): same rule as before #29.
uint64_t PageHash(const void* page);  // 64-bit, SSE2; page-aligned 4 KB
bool IsZero(const void* page);

struct Stats {
  uint64_t chunkBytes = 0;   // page-file bytes of the store (chunks)
  uint64_t liveBytes = 0;    // compressed bytes still referenced
  uint32_t chunks = 0, entries = 0;
  uint64_t packedPages = 0, zeroPages = 0, sharedPages = 0, newPages = 0;  // totals since the start
  double packMs = 0;          // last job
  std::string last;           // last job's line
  uint64_t compactedBytes = 0;
};
Stats GetStats();

// Tests: ss pack 0|1 (0 = slots stay raw: the old behaviour, for the before/after numbers).
void SetEnabled(bool on);
bool Enabled();

}  // namespace ssstore
