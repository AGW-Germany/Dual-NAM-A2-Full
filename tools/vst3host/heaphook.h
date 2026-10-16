// Counts heap operations made by the plugin DLL, by patching the plugin's import
// table entries for KERNEL32!HeapAlloc / HeapFree / HeapReAlloc.
//
// The plugin links the C runtime statically, so every malloc / new / Eigen
// allocation inside it ends up in those three imports. Patching only the
// plugin's own import table means the harness's allocations are never counted.
#pragma once

#include <atomic>
#include <cstdint>

namespace heaphook
{
struct Counts
{
  // Made on a thread while it is "armed" (the audio thread, inside process()).
  std::atomic<int64_t> armedAlloc{0}, armedFree{0}, armedRealloc{0};
  // Everything the plugin did, any thread. alloc - free = blocks outstanding.
  std::atomic<int64_t> totalAlloc{0}, totalFree{0};
};

extern Counts gCounts;

// Patch the import table of the module whose file name is moduleFileName
// (e.g. "DualNAM_A2_Full.vst3"). Returns false if any import was not found.
bool Install(const wchar_t* moduleFileName);

// Arm / disarm counting for the calling thread.
void Arm(bool on);

inline int64_t Outstanding() { return gCounts.totalAlloc.load() - gCounts.totalFree.load(); }
inline int64_t ArmedOps()
{
  return gCounts.armedAlloc.load() + gCounts.armedFree.load() + gCounts.armedRealloc.load();
}
inline void ResetArmed()
{
  gCounts.armedAlloc = 0;
  gCounts.armedFree = 0;
  gCounts.armedRealloc = 0;
}
} // namespace heaphook
